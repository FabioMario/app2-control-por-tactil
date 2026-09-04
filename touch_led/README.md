| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# touch_led — App 2 (Clase 2)

Firmware base de la **App 2** de la Clase 2. Los **botones táctiles
capacitivos** de la placa de extensión **ESP-LyraP-TouchA** gobiernan el
**LED RGB** de la **ESP32-S2-Kaluga-1** **mediante interrupciones**, y cada
evento se publica por el monitor serie.

```
   dedo ──▶ periférico táctil ──▶ callback on_active (ISR)
                                        │ encola {canal, activo, marca_us}
                                        ▼
                                     tarea ──▶ máquina de estados ──▶ LED
```

## Hardware

- **ESP-LyraP-TouchA** conectada al **Touch FPC Connector** de la Kaluga-1
  con el cable plano de 20 pines.
- **Microinterruptores T1–T14** de la cara inferior de la Kaluga-1 en **OFF**:
  así GPIO1–GPIO14 quedan dedicados a los sensores táctiles.
- **LED RGB direccionable** en **GPIO45**.
  ⚠️ **Coloca el jumper RGB** en la placa: sin él, el LED no recibe señal.
- Target ESP-IDF: **`esp32s2`** (touch sensor **V2**, canales T1–T14 sobre
  GPIO1–GPIO14).

## Mapa de botones

| Serigrafía | Canal | GPIO | Acción |
|---|:-:|:-:|---|
| **RECORD** | T5 | 5 | Encender / apagar |
| **PLAY** | T2 | 2 | Siguiente color de la paleta |
| **VOL_UP** | T1 | 1 | Subir brillo |
| **VOL_DOWN** | T3 | 3 | Bajar brillo |
| **PHOTO** | T6 | 6 | Alternar modo fijo ↔ parpadeo |
| **NETWORK** | T11 | 11 | Publicar estado (*uptime*, *heap*, eventos) |

> Conflictos conocidos del kit: T1/T2/T3 con la cámara, T6 con los botones de
> la placa de audio y T11 con el *chip select* del LCD. Con solo la
> ESP-LyraP-TouchA conectada no hay problema.

## Estructura

```
touch_led/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml      Declara la dependencia espressif/led_strip
│   └── touch_led_main.c
└── README.md   (este archivo)
```

## Cómo compilar, flashear y monitorear

```bash
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

- Sustituye `/dev/cu.usbserial-XXXX` por tu puerto (ver `ls /dev/cu.*` en macOS).
- Conecta el cable al puerto **UART** de la placa.
- Salir del monitor: `Ctrl + ]`.

## Qué hace

1. **Configura el LED RGB** (GPIO45) con el componente `led_strip` (RMT).
2. **Crea el controlador táctil** y un **canal por botón** con la
   configuración de muestreo del *touch V2*.
3. **Calibra**: hace tres barridos de calentamiento, lee el `benchmark` real de
   cada canal y fija `active_thresh = benchmark × 2 %`.
4. **Registra los callbacks** `on_active` / `on_inactive`, que se ejecutan en
   **contexto de interrupción** y solo **encolan** `{canal, activo, marca_us}`.
5. **La tarea** consume la cola, calcula la latencia ISR → tarea, actualiza la
   máquina de estados (encendido, color, brillo, modo) y refresca el LED.

> ⚠️ **No toques la placa durante el arranque.** Si el `benchmark` inicial se
> mide con el dedo apoyado, ese botón no se activará hasta el siguiente reset.

## Salida esperada

```
I (xxx) touch_led: Control tactil del LED RGB — ESP32-S2-Kaluga-1
I (xxx) touch_led: Calibrando... NO toques la placa durante el arranque
I (xxx) touch_led:   RECORD   T5   benchmark=41230  umbral=824
I (xxx) touch_led:   PLAY     T2   benchmark=40887  umbral=817
I (xxx) touch_led:   VOL_UP   T1   benchmark=39954  umbral=799
I (xxx) touch_led:   VOL_DOWN T3   benchmark=41002  umbral=820
I (xxx) touch_led:   PHOTO    T6   benchmark=40551  umbral=811
I (xxx) touch_led:   NETWORK  T11  benchmark=42118  umbral=842
I (xxx) touch_led: Listo. RECORD=on/off  PLAY=color  VOL+/VOL-=brillo  PHOTO=modo  NETWORK=estado
I (xxx) touch_led: [0] PLAY     (T2) PULSADO | latencia ISR->tarea: 38 us
I (xxx) touch_led:   -> color VERDE
I (xxx) touch_led: [1] PLAY     (T2) SUELTO  | latencia ISR->tarea: 36 us
```

Los valores de `benchmark` dependen de tu placa: lo importante es que sean
**estables** entre reinicios.

## Ajuste del umbral

`UMBRAL_RATIO` (2 % por defecto) es el único parámetro que normalmente hay que
tocar:

| Síntoma | Ajuste |
|---|---|
| No detecta el toque | Bajar a `0.01f` |
| Se dispara solo | Subir a `0.03f`–`0.05f` |
| Detecta al acercar la mano | Bajar `charge_speed` |
| Un canal siempre activo | Se calibró con el dedo encima: reiniciar sin tocar |
| Todos los canales a la vez | Placa mojada: usar el canal GUARD (T4) |

## Ideas para extender (App 2)

- **Pulsación larga** vs. corta midiendo el tiempo entre `on_active` y
  `on_inactive`.
- **Deslizamiento**: detectar la secuencia VOL_DOWN → PLAY → VOL_UP.
- Combinación de **dos botones simultáneos** usando `status_mask` del evento.
- Publicar la **latencia máxima** observada cada N eventos.
- Usar el canal **GUARD (T4)** para detectar la placa mojada y desactivar el
  resto de botones.
- Efecto de **respiración** aprovechando el *timeout* de `xQueueReceive`.

## Solución de problemas

| Síntoma | Solución |
|---|---|
| Ningún botón responde | Microinterruptores **T1–T14 en OFF**; cable FPC bien insertado |
| El LED no enciende | Colocar el **jumper RGB**; verificar GPIO45 |
| No compila (`touch_sens.h`) | Requiere **ESP-IDF ≥ v5.4**; el driver antiguo `touch_sensor.h` no existe en v6 |
| No compila (`led_strip.h`) | Comprobar la conexión a Internet en el primer *build* |
| `Failed to connect` al flashear | Secuencia **BOOT + RST** (modo bootloader) |
| Eventos duplicados | Es normal: llega uno al pulsar y otro al soltar |

## Referencias

- [../../detalles/touch-capacitivo-esp32s2.md](../../detalles/touch-capacitivo-esp32s2.md)
- [../../detalles/interrupciones-y-datos-compartidos.md](../../detalles/interrupciones-y-datos-compartidos.md)
- Espressif, *ESP-IDF Programming Guide — Capacitive Touch Sensor (ESP32-S2)*.
- Espressif, *ESP-LyraP-TouchA v1.1 User Guide*.
- Guía de conexión e instalación:
  [../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md](../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md)
