# Premium Air Quality Station Firmware

PlatformIO firmware for an M5Stack CoreS3-Lite with a Sensirion SEN66 on Grove Port A.

## Build

Install PlatformIO, then run:

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
pio run
pio run --target upload
pio device monitor
```

The UTF-8 environment variables avoid a known Windows/PlatformIO dependency-tree output issue on CP1252 consoles.

This project builds successfully with PlatformIO Core 6.1.19 against `board = m5stack-cores3`.

## Hardware Notes

- SEN66 I2C: SDA GPIO 2, SCL GPIO 1, address `0x6B`.
- Grove VCC is 5 V; power the Adafruit 6331 input from Grove VCC and let the breakout LDO provide 3.3 V for the SEN66.
- microSD SPI follows the CoreS3 pin map: MISO GPIO 35, MOSI GPIO 37, SCK GPIO 36, CS GPIO 4.
- GC0308 camera uses the current M5Stack CoreS3 RGB565 camera path, then compresses to JPEG before saving.
- The LTR-553 interrupt GPIO is left as `-1` in `main.cpp` because M5Stack does not expose a clear ESP32 GPIO for it in the public pin table. Proximity is still polled and used for display wake/camera capture.

## Modules

- `config.*`: SD init, `/config.json`, directory creation.
- `sensor_manager.*`: SEN66 init, warmup, standby, averaged reads, CO2 calibration.
- `logger.*`: 24-byte daily binary records, 48-byte hourly summaries, downsampled history reads, alert log.
- `ui_manager.*`: LVGL dark glass UI, swipe screens, dashboard, chart, event log, settings/camera roll.
- `audio_manager.*`: M5Unified I2S speaker alarm chime and mute behavior.
- `camera_manager.*`: CoreS3 GC0308 capture, RGB565-to-JPEG save, 50-file rotation.
- `ftp_manager.*`: SimpleFTPServer wrapper.
- `main.cpp`: FreeRTOS acquisition/network tasks, alarm state, UI service loop, light-sleep handling.
