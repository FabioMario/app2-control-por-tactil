/*
 * touch_led — Clase 2 (Sistemas Embebidos, UCU 2026)
 *
 * Base de la App 2: los botones táctiles capacitivos de la placa de extensión
 * ESP-LyraP-TouchA gobiernan el LED RGB de la ESP32-S2-Kaluga-1 mediante
 * INTERRUPCIONES.
 *
 * Arquitectura:
 *
 *      callback on_active/on_inactive  --cola-->  tarea  -->  LED + log
 *      (contexto de interrupción)                 (estado de la aplicación)
 *
 * Los callbacks del driver de táctil se ejecutan en contexto de interrupción:
 * solo encolan el evento. Toda la máquina de estados vive dentro de la tarea,
 * así que no hay ningún dato compartido que proteger.
 *
 * Hardware:
 *   - ESP-LyraP-TouchA conectada al Touch FPC Connector con el cable de 20 pines.
 *   - Microinterruptores T1-T14 de la cara inferior en OFF (pines dedicados
 *     al táctil).
 *   - LED RGB direccionable en GPIO45 (requiere el JUMPER RGB colocado).
 *   - Target: esp32s2 (touch sensor V2, canales T1-T14 sobre GPIO1-GPIO14).
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "led_strip.h"

#define RGB_LED_GPIO            45
#define RGB_LED_COUNT           1

/* Barridos de calentamiento antes de leer la línea base. */
#define BARRIDOS_CALIBRACION    3
/* Tope de tiempo para la medida de un canal. La configuración por defecto del
 * driver lo deja en 0, que significa "sin límite", y eso es justo lo que cuelga
 * el arranque: si un pad no termina de cargarse (cable FPC flojo, un
 * microinterruptor T1-T14 en ON, o el pin gobernado por la cámara o el LCD), la
 * máquina de estados del táctil se queda en ese canal para siempre y el barrido
 * no acaba nunca. Con un tope, el hardware lanza una interrupción de expiración
 * y pasa al canal siguiente. Un pad sano mide en unos 2-5 ms, así que sobra
 * margen. */
#define MEDIDA_MAX_US           50000
/* Presupuesto de tiempo de un barrido completo sobre todos los canales. */
#define BARRIDO_TIMEOUT_MS      2000
/* Umbral de activación como fracción del benchmark medido. Súbelo si se
 * dispara solo; bájalo si no detecta el toque. */
#define UMBRAL_RATIO            0.02f
/* Valor de arranque, se reemplaza tras la calibración. */
#define UMBRAL_INICIAL          2000
#define PARPADEO_MS             300
#define COLA_LARGO              16

static const char *TAG = "touch_led";

typedef enum {
    ACC_ENCENDIDO = 0,
    ACC_COLOR,
    ACC_BRILLO_MAS,
    ACC_BRILLO_MENOS,
    ACC_MODO,
    ACC_ESTADO,
} accion_t;

typedef struct {
    int canal;              /* número de canal táctil == número de GPIO */
    const char *nombre;     /* serigrafía de la placa ESP-LyraP-TouchA */
    accion_t accion;
} boton_t;

/* Mapa de la ESP-LyraP-TouchA. T4 (GUARD) y T14 (SHIELD) no son botones:
 * son el anillo de guarda y el blindaje del sistema antiagua. */
static const boton_t BOTONES[] = {
    {  5, "RECORD",   ACC_ENCENDIDO    },
    {  2, "PLAY",     ACC_COLOR        },
    {  1, "VOL_UP",   ACC_BRILLO_MAS   },
    {  3, "VOL_DOWN", ACC_BRILLO_MENOS },
    {  6, "PHOTO",    ACC_MODO         },
    { 11, "NETWORK",  ACC_ESTADO       },
};
#define BOTONES_N   (sizeof(BOTONES) / sizeof(BOTONES[0]))

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char *nombre;
} color_t;

static const color_t PALETA[] = {
    { 255,   0,   0, "ROJO"    },
    {   0, 255,   0, "VERDE"   },
    {   0,   0, 255, "AZUL"    },
    { 255, 200,   0, "AMBAR"   },
    { 255,   0, 255, "MAGENTA" },
    {   0, 255, 255, "CIAN"    },
    { 255, 255, 255, "BLANCO"  },
};
#define PALETA_N    (sizeof(PALETA) / sizeof(PALETA[0]))

/* Escala de brillo perceptualmente espaciada (aproximadamente logarítmica). */
static const uint8_t NIVELES[] = { 2, 4, 8, 16, 32, 64, 128, 255 };
#define NIVELES_N   (sizeof(NIVELES) / sizeof(NIVELES[0]))

/* Evento que la ISR entrega a la tarea. */
typedef struct {
    int canal;
    bool activo;
    int64_t marca_us;
} evento_t;

/* Estado de la aplicación: solo lo toca la tarea. */
typedef struct {
    bool encendido;
    uint8_t color;      /* índice en PALETA  */
    uint8_t brillo;     /* índice en NIVELES */
    bool parpadeo;
    bool fase;          /* fase visible del parpadeo */
} estado_t;

static QueueHandle_t s_cola;
static touch_channel_handle_t s_canales[BOTONES_N];
/* Un bit por número de canal: los canales cuya medida superó MEDIDA_MAX_US.
 * Lo escribe la interrupción y lo lee la calibración. */
static volatile uint32_t s_expirados;

/* ------------------------------------------------------------------ ISR --- */

/* Se ejecuta en contexto de interrupción: nada de logs, nada de bloqueos.
 * El bool devuelto es el "need yield": el driver hace el cambio de contexto. */
static bool IRAM_ATTR al_tocar(touch_sensor_handle_t sensor,
                               const touch_active_event_data_t *ev,
                               void *ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    const evento_t e = {
        .canal = ev->chan_id,
        .activo = true,
        .marca_us = esp_timer_get_time(),
    };
    xQueueSendFromISR(s_cola, &e, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

static bool IRAM_ATTR al_soltar(touch_sensor_handle_t sensor,
                                const touch_inactive_event_data_t *ev,
                                void *ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    const evento_t e = {
        .canal = ev->chan_id,
        .activo = false,
        .marca_us = esp_timer_get_time(),
    };
    xQueueSendFromISR(s_cola, &e, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

/* Un canal que supera MEDIDA_MAX_US no ha dado una lectura válida. Aquí solo se
 * anota cuál fue: la calibración ya se encarga de avisar y de descartarlo. */
static bool IRAM_ATTR al_expirar(touch_sensor_handle_t sensor,
                                 const touch_timeout_event_data_t *ev,
                                 void *ctx)
{
    s_expirados |= 1UL << ev->chan_id;
    return false;
}

/* ------------------------------------------------------------ periféricos - */

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

static touch_channel_config_t config_canal_por_defecto(void)
{
    touch_channel_config_t cfg = {
        .active_thresh = { UMBRAL_INICIAL },
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    return cfg;
}

/* Mide la línea base real de cada canal y fija el umbral como un porcentaje
 * de ella. Un umbral copiado de un ejemplo casi nunca sirve: depende del
 * electrodo, del grosor del plástico y hasta de la caja.
 * Un canal que no responde se descarta; nunca es motivo para abortar el
 * arranque: un pad averiado no puede llevarse por delante los otros cinco. */
static void calibrar_umbrales(touch_sensor_handle_t sensor)
{
    ESP_LOGI(TAG, "Calibrando... NO toques la placa durante el arranque");

    s_expirados = 0;
    ESP_ERROR_CHECK(touch_sensor_enable(sensor));
    for (int i = 0; i < BARRIDOS_CALIBRACION; i++) {
        /* Un barrido todavía puede volver incompleto; los demás canales sí se
         * han medido, así que esto es un aviso y no un panic. */
        const esp_err_t err =
            touch_sensor_trigger_oneshot_scanning(sensor, BARRIDO_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Barrido %d incompleto: %s", i, esp_err_to_name(err));
        }
    }
    ESP_ERROR_CHECK(touch_sensor_disable(sensor));

    size_t vivos = 0;
    for (size_t i = 0; i < BOTONES_N; i++) {
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = { 0 };
        ESP_ERROR_CHECK(touch_channel_read_data(s_canales[i],
                                                TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                                benchmark));

        /* Sin línea base no hay umbral utilizable: benchmark * ratio daría 0 y
         * el canal reportaría toque en cada barrido. Se libera, y de paso deja
         * de frenar el escaneo. */
        if ((s_expirados & (1UL << BOTONES[i].canal)) || benchmark[0] == 0) {
            ESP_LOGW(TAG, "  %-8s T%-2d  sin respuesta -> boton desactivado",
                     BOTONES[i].nombre, BOTONES[i].canal);
            ESP_ERROR_CHECK(touch_sensor_del_channel(s_canales[i]));
            s_canales[i] = NULL;
            continue;
        }

        touch_channel_config_t cfg = config_canal_por_defecto();
        cfg.active_thresh[0] = (uint32_t)(benchmark[0] * UMBRAL_RATIO);
        ESP_ERROR_CHECK(touch_sensor_reconfig_channel(s_canales[i], &cfg));
        vivos++;

        ESP_LOGI(TAG, "  %-8s T%-2d  benchmark=%" PRIu32 "  umbral=%" PRIu32,
                 BOTONES[i].nombre, BOTONES[i].canal,
                 benchmark[0], cfg.active_thresh[0]);
    }

    if (vivos == 0) {
        ESP_LOGE(TAG, "Ningun canal respondio: revisa el cable FPC de la "
                      "ESP-LyraP-TouchA y los microinterruptores T1-T14 (en OFF)");
    }
}

static touch_sensor_handle_t configurar_tactil(void)
{
    touch_sensor_sample_config_t muestreo[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5,
                                                   TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, muestreo);
    /* El valor por defecto es 0 (sin límite), y con él basta un pad que no
     * responda para colgar la máquina de estados del táctil y, con ella, todo
     * el arranque. */
    sens_cfg.max_meas_time_us = MEDIDA_MAX_US;

    touch_sensor_handle_t sensor = NULL;
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &sensor));

    touch_channel_config_t chan_cfg = config_canal_por_defecto();
    for (size_t i = 0; i < BOTONES_N; i++) {
        ESP_ERROR_CHECK(touch_sensor_new_channel(sensor, BOTONES[i].canal,
                                                 &chan_cfg, &s_canales[i]));
    }

    touch_sensor_filter_config_t filtro = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(sensor, &filtro));

    /* Durante la calibración, solo el callback de expiración: on_active se
     * dispararía con el umbral provisional e inundaría la cola antes de que
     * exista la tarea. Los callbacks solo se pueden registrar con el sensor
     * deshabilitado. */
    touch_event_callbacks_t solo_expiracion = {
        .on_timeout = al_expirar,
    };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(sensor, &solo_expiracion, NULL));

    calibrar_umbrales(sensor);

    touch_event_callbacks_t callbacks = {
        .on_active = al_tocar,
        .on_inactive = al_soltar,
        .on_timeout = al_expirar,
    };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(sensor, &callbacks, NULL));

    ESP_ERROR_CHECK(touch_sensor_enable(sensor));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(sensor));
    return sensor;
}

/* ----------------------------------------------------------------- tarea -- */

static int indice_de_canal(int canal)
{
    for (size_t i = 0; i < BOTONES_N; i++) {
        if (BOTONES[i].canal == canal) {
            return (int)i;
        }
    }
    return -1;
}

static void aplicar_al_led(led_strip_handle_t led, const estado_t *e)
{
    uint8_t r = 0, g = 0, b = 0;

    if (e->encendido && (!e->parpadeo || e->fase)) {
        const color_t *c = &PALETA[e->color];
        const uint8_t nivel = NIVELES[e->brillo];
        r = (uint8_t)((c->r * nivel) / 255);
        g = (uint8_t)((c->g * nivel) / 255);
        b = (uint8_t)((c->b * nivel) / 255);
    }

    ESP_ERROR_CHECK(led_strip_set_pixel(led, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(led));
}

static void ejecutar_accion(estado_t *e, accion_t accion, uint32_t eventos)
{
    switch (accion) {
    case ACC_ENCENDIDO:
        e->encendido = !e->encendido;
        ESP_LOGI(TAG, "  -> LED %s", e->encendido ? "ENCENDIDO" : "APAGADO");
        break;

    case ACC_COLOR:
        e->color = (uint8_t)((e->color + 1) % PALETA_N);
        ESP_LOGI(TAG, "  -> color %s", PALETA[e->color].nombre);
        break;

    case ACC_BRILLO_MAS:
        if (e->brillo + 1 < NIVELES_N) {
            e->brillo++;
        }
        ESP_LOGI(TAG, "  -> brillo %u/%u (%u)", e->brillo + 1u,
                 (unsigned)NIVELES_N, NIVELES[e->brillo]);
        break;

    case ACC_BRILLO_MENOS:
        if (e->brillo > 0) {
            e->brillo--;
        }
        ESP_LOGI(TAG, "  -> brillo %u/%u (%u)", e->brillo + 1u,
                 (unsigned)NIVELES_N, NIVELES[e->brillo]);
        break;

    case ACC_MODO:
        e->parpadeo = !e->parpadeo;
        e->fase = true;
        ESP_LOGI(TAG, "  -> modo %s", e->parpadeo ? "PARPADEO" : "FIJO");
        break;

    case ACC_ESTADO:
        ESP_LOGI(TAG, "  -> estado: %s, %s, brillo %u, modo %s",
                 e->encendido ? "ON" : "OFF", PALETA[e->color].nombre,
                 e->brillo + 1u, e->parpadeo ? "parpadeo" : "fijo");
        ESP_LOGI(TAG, "  -> uptime %" PRId64 " ms, heap libre %" PRIu32
                 " B, eventos %" PRIu32,
                 esp_timer_get_time() / 1000, esp_get_free_heap_size(), eventos);
        break;
    }
}

static void tarea_tactil(void *arg)
{
    led_strip_handle_t led = (led_strip_handle_t)arg;
    estado_t estado = {
        .encendido = true,
        .color = 0,
        .brillo = 3,
        .parpadeo = false,
        .fase = true,
    };
    uint32_t eventos = 0;
    evento_t ev;

    aplicar_al_led(led, &estado);

    while (1) {
        /* En modo parpadeo la tarea despierta por evento O por vencimiento del
         * plazo; en modo fijo duerme indefinidamente y no consume CPU. */
        const TickType_t espera = estado.parpadeo ? pdMS_TO_TICKS(PARPADEO_MS)
                                                  : portMAX_DELAY;

        if (xQueueReceive(s_cola, &ev, espera) == pdTRUE) {
            const int idx = indice_de_canal(ev.canal);
            if (idx < 0) {
                continue;
            }

            ESP_LOGI(TAG, "[%" PRIu32 "] %-8s (T%d) %-7s | latencia ISR->tarea: %"
                     PRId64 " us",
                     eventos, BOTONES[idx].nombre, ev.canal,
                     ev.activo ? "PULSADO" : "SUELTO",
                     esp_timer_get_time() - ev.marca_us);
            eventos++;

            if (ev.activo) {
                ejecutar_accion(&estado, BOTONES[idx].accion, eventos);
                aplicar_al_led(led, &estado);
            }
        } else {
            estado.fase = !estado.fase;
            aplicar_al_led(led, &estado);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Control tactil del LED RGB — ESP32-S2-Kaluga-1");

    led_strip_handle_t led = configurar_led();

    s_cola = xQueueCreate(COLA_LARGO, sizeof(evento_t));
    ESP_ERROR_CHECK(s_cola != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* La cola debe existir antes de que se registren los callbacks. */
    configurar_tactil();

    ESP_LOGI(TAG, "Listo. RECORD=on/off  PLAY=color  VOL+/VOL-=brillo"
                  "  PHOTO=modo  NETWORK=estado");

    xTaskCreate(tarea_tactil, "tactil", 4096, led, 10, NULL);
}
