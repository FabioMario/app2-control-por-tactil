/*
 * gpio_boton — Clase 2 (Sistemas Embebidos, UCU 2026)
 *
 * GPIO de entrada con interrupción por flanco: cada pulsación del botón BOOT
 * (GPIO0) de la ESP32-S2-Kaluga-1 avanza el color del LED RGB y publica el
 * evento por el monitor serie.
 *
 * Demuestra el patrón que usaremos siempre:
 *
 *      ISR (µs)  --encola-->  Tarea (ms)  -->  trabajo real
 *
 * La ISR solo anota el instante del flanco y descarta rebotes; el manejo del
 * LED y los mensajes ocurren en una tarea, con las interrupciones habilitadas.
 *
 * Hardware:
 *   - Botón BOOT en GPIO0, a masa, con pull-up interno: activo en BAJO.
 *   - LED RGB direccionable en GPIO45 (requiere el JUMPER RGB colocado).
 *   - Target: esp32s2.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

/* Botón BOOT de la Kaluga-1. También es pin de strapping: si está pulsado
 * durante el reset, el chip arranca en modo descarga. En marcha es un GPIO
 * normal. */
#define BOTON_GPIO        GPIO_NUM_0
/* LED RGB direccionable de la placa (ver User Guide del kit). */
#define RGB_LED_GPIO      GPIO_NUM_45
#define RGB_LED_COUNT     1
/* Ventana de guarda del antirrebote: los flancos que lleguen antes se
 * descartan. Un pulsador mecánico rebota durante 1-20 ms. */
#define REBOTE_US         50000
#define COLA_LARGO        8

static const char *TAG = "gpio_boton";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char *nombre;
} color_t;

/* static const: la tabla vive en .rodata (Flash), no gasta RAM. */
static const color_t PALETA[] = {
    { 32,  0,  0, "ROJO"    },
    {  0, 32,  0, "VERDE"   },
    {  0,  0, 32, "AZUL"    },
    { 32, 24,  0, "AMARILLO"},
    { 16, 16, 16, "BLANCO"  },
};
#define PALETA_N  (sizeof(PALETA) / sizeof(PALETA[0]))

static QueueHandle_t s_cola;

/* Solo la ISR escribe y lee esta marca, así que no hay condición de carrera
 * pese a ser de 64 bits. */
static volatile int64_t s_ultimo_us;

/* Un único escritor (la ISR) y un único lector (la tarea): en Xtensa el acceso
 * a un uint32_t alineado es atómico, así que basta con volatile. */
static volatile uint32_t s_rebotes;

/* IRAM_ATTR: la ISR debe poder ejecutarse aunque la caché de Flash esté
 * desactivada (por ejemplo, durante una escritura en NVS). */
static void IRAM_ATTR isr_boton(void *arg)
{
    const int64_t ahora = esp_timer_get_time();

    if (ahora - s_ultimo_us < REBOTE_US) {
        s_rebotes++;
        return;
    }
    s_ultimo_us = ahora;

    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(s_cola, &ahora, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        /* Cede la CPU al salir de la ISR para que la tarea despierte ya, sin
         * esperar al siguiente tick del planificador. */
        portYIELD_FROM_ISR();
    }
}

static led_strip_handle_t configurar_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };

    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    return led_strip;
}

static void configurar_boton(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* nivel alto en reposo */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,    /* flanco de bajada: al pulsar */
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* Instala la ISR global del driver de GPIO; se llama una sola vez en todo
     * el programa, y luego se registra un manejador por pin. */
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOTON_GPIO, isr_boton, NULL));
}

/* El estado del LED vive dentro de la tarea: al no compartirlo con la ISR no
 * hace falta ni volatile ni sección crítica. */
static void tarea_boton(void *arg)
{
    led_strip_handle_t led = (led_strip_handle_t)arg;
    uint32_t pulsaciones = 0;
    int64_t latencia_max_us = 0;
    int64_t marca_us;

    while (xQueueReceive(s_cola, &marca_us, portMAX_DELAY) == pdTRUE) {
        const int64_t latencia_us = esp_timer_get_time() - marca_us;
        if (latencia_us > latencia_max_us) {
            latencia_max_us = latencia_us;
        }

        const color_t *c = &PALETA[pulsaciones % PALETA_N];
        ESP_ERROR_CHECK(led_strip_set_pixel(led, 0, c->r, c->g, c->b));
        ESP_ERROR_CHECK(led_strip_refresh(led));

        ESP_LOGI(TAG,
                 "[%" PRIu32 "] BOOT -> %-8s | latencia ISR->tarea: %" PRId64
                 " us (max %" PRId64 ") | rebotes descartados: %" PRIu32,
                 pulsaciones, c->nombre, latencia_us, latencia_max_us, s_rebotes);
        pulsaciones++;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "GPIO e interrupciones en la ESP32-S2-Kaluga-1");
    ESP_LOGI(TAG, "Pulsa el boton BOOT (GPIO%d) para avanzar el color del LED",
             BOTON_GPIO);

    led_strip_handle_t led = configurar_led();

    s_cola = xQueueCreate(COLA_LARGO, sizeof(int64_t));
    ESP_ERROR_CHECK(s_cola != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* La cola debe existir antes de habilitar la interrupción. */
    configurar_boton();

    xTaskCreate(tarea_boton, "boton", 3072, led, 10, NULL);
}
