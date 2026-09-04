| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# gpio_boton — GPIO e interrupciones (Clase 2)

Primer ejemplo de la Clase 2. Configura el botón **BOOT (GPIO0)** de la
**ESP32-S2-Kaluga-1** como entrada con **interrupción por flanco de bajada**.
Cada pulsación avanza el color del **LED RGB** y publica el evento por el
monitor serie.

El objetivo no es el LED: es el **patrón de trabajo con interrupciones**.

```
   flanco ──▶ ISR (µs)  ──encola──▶  Tarea (ms)  ──▶  LED + log
              ¿pasó algo?             ¿qué hago?
```

## Hardware

- **Botón BOOT** en **GPIO0**, conectado a masa, con *pull-up* interno
  habilitado por software: en reposo lee `1`, pulsado lee `0`.
- **LED RGB direccionable** en **GPIO45**.
  ⚠️ **Coloca el jumper RGB** en la placa: sin él, el LED no recibe señal.
- Target ESP-IDF: **`esp32s2`**.

> GPIO0 también es un pin de *strapping*: si está pulsado durante el **reset**,
> el chip arranca en **modo descarga**. En ejecución es un GPIO normal.

## Estructura

```
gpio_boton/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml      Declara la dependencia espressif/led_strip
│   └── gpio_boton_main.c
└── README.md   (este archivo)
```

## Cómo compilar, flashear y monitorear

Con la extensión **ESP-IDF para VS Code** usa los botones de la barra
inferior, o desde la terminal de ESP-IDF:

```bash
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

- Sustituye `/dev/cu.usbserial-XXXX` por tu puerto (ver `ls /dev/cu.*` en macOS).
- Conecta el cable al puerto **UART** de la placa.
- Salir del monitor: `Ctrl + ]`.

## Qué hace, línea a línea

1. **Configura el pin** con `gpio_config()`: entrada, *pull-up* activado,
   interrupción por flanco de bajada (`GPIO_INTR_NEGEDGE`).
2. **Instala la ISR global** del driver con `gpio_install_isr_service()` —una
   sola vez en todo el programa— y registra el manejador del pin con
   `gpio_isr_handler_add()`.
3. **La ISR** (`IRAM_ATTR`) hace tres cosas y nada más:
   - lee `esp_timer_get_time()` para marcar el instante del flanco;
   - descarta el flanco si han pasado menos de 50 ms desde el anterior
     (**antirrebote**) e incrementa el contador de rebotes;
   - encola la marca con `xQueueSendFromISR()` y hace `portYIELD_FROM_ISR()`
     si ha despertado una tarea de mayor prioridad.
4. **La tarea** recibe la marca, calcula la **latencia ISR → tarea**, cambia
   el color del LED y publica todo por el serie.

## Salida esperada

```
I (xxx) gpio_boton: GPIO e interrupciones en la ESP32-S2-Kaluga-1
I (xxx) gpio_boton: Pulsa el boton BOOT (GPIO0) para avanzar el color del LED
I (xxx) gpio_boton: [0] BOOT -> ROJO     | latencia ISR->tarea: 42 us (max 42) | rebotes descartados: 0
I (xxx) gpio_boton: [1] BOOT -> VERDE    | latencia ISR->tarea: 39 us (max 42) | rebotes descartados: 3
I (xxx) gpio_boton: [2] BOOT -> AZUL     | latencia ISR->tarea: 40 us (max 42) | rebotes descartados: 7
```

El contador de **rebotes descartados** crece con cada pulsación: son los
flancos espurios que el filtro temporal ha eliminado. Es la prueba
experimental de que el *bouncing* existe.

## Cosas para probar

| Experimento | Qué observar |
|---|---|
| Poner `REBOTE_US` a `0` | Cada pulsación produce varios eventos |
| Cambiar a `GPIO_INTR_ANYEDGE` | Se detectan también las sueltas |
| Bajar la prioridad de la tarea a 1 | La latencia ISR → tarea crece |
| Quitar `IRAM_ATTR` de la ISR | Sigue funcionando… hasta que se escriba la Flash |
| Añadir `ESP_LOGI` dentro de la ISR | El sistema se vuelve inestable (**no lo dejes así**) |

## Solución de problemas

| Síntoma | Solución |
|---|---|
| El LED no enciende | Colocar el **jumper RGB**; verificar GPIO45 |
| No se detecta la pulsación | Comprobar que se usa el botón **BOOT**, no **RST** |
| Varios eventos por pulsación | Subir `REBOTE_US` |
| No compila (`led_strip.h`) | Comprobar la conexión a Internet en el primer *build* |
| `Failed to connect` al flashear | Secuencia **BOOT + RST** (modo bootloader) |

## Referencias

- [../../detalles/interrupciones-y-datos-compartidos.md](../../detalles/interrupciones-y-datos-compartidos.md)
- Espressif, *ESP-IDF Programming Guide — GPIO & RTC GPIO (ESP32-S2)*.
- Guía de conexión e instalación:
  [../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md](../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md)
