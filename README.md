# Premium Air Quality Station Firmware

PlatformIO firmware for an M5Stack CoreS3-Lite with a Sensirion SEN66 on Grove Port A.

## Feature Overview

This firmware turns the CoreS3-Lite into a compact premium air-quality station with live SEN66 readings, long-term binary logging, alarm handling, camera snapshots, and optional network publishing.

- Measures PM1.0, PM2.5, PM4.0, PM10, CO2, VOC Index, NOx Index, temperature, and relative humidity.
- Uses SEN66 warmup-aware measurement scheduling so low-power cycles do not produce stale startup readings.
- Stores fixed-width daily binary records and hourly monthly summaries for fast chart loading on a microcontroller.
- Presents a dark LVGL interface with glass-style cards, horizontal swipe navigation, alarm banner, and touch controls.
- Publishes MQTT payloads when Wi-Fi is available while continuing local SD logging when offline.
- Plays alarms through the CoreS3 I2S speaker using `M5.Speaker`, not PWM buzzer output.
- Captures proximity-triggered camera snapshots, stores JPEG files on SD, and rotates the camera folder to the newest 50 images.
- Provides optional FTP access to the SD card when enabled in settings.

## UI Mockups

These are documentation-grade mockups generated from the current LVGL layout. They are not hardware screenshots, but they match the 320x240 screen structure, control placement, and visual hierarchy used in `ui_manager.cpp`.

| Dashboard | Historical Charts |
| --- | --- |
| ![Dashboard screen mockup](docs/mockups/dashboard.svg) | ![Historical charts screen mockup](docs/mockups/history.svg) |

| Event Log | Settings and Camera Roll |
| --- | --- |
| ![Event log screen mockup](docs/mockups/events.svg) | ![Settings and camera roll screen mockup](docs/mockups/settings.svg) |

## Screen Walkthrough

### Screen 1: Dashboard

The dashboard is the default screen after boot. It shows a compact top status bar with Wi-Fi, MQTT, SD, time, and device state. The large circular gauge focuses on PM2.5 as the primary pollutant signal, using the AQI tint color: green for good, amber for warning, and red for unhealthy.

The right side uses six glass cards for CO2, temperature, humidity, VOC, NOx, and PM10. When an alarm condition is active, a red `SILENCE ALARM` banner appears across the bottom. Tapping it mutes the I2S alarm while keeping the banner visible until air quality returns below the configured thresholds.

### Screen 2: Historical Charts

The history screen uses `lv_chart` to render downsampled data without loading large files into memory. The `24 Hours` mode reads the current daily binary log and strides it down to roughly 120 display points. The `30 Days` mode reads the precomputed hourly summary file instead of parsing every raw measurement.

Rolling mean, minimum, and maximum values are shown below the chart. This screen is designed to stay responsive even after weeks of logging.

### Screen 3: Event Log

The event log is a scrollable LVGL list of persisted anomalies and system events. Examples include high PM2.5, high CO2, VOC alarm, sensor communication failure, and recovery messages.

Events are also appended to `/alerts.log` on the SD card, so the visible list is backed by a durable text log for later inspection.

### Screen 4: Settings and Camera Roll

The settings screen exposes brightness, alarm volume, FTP enable/disable, and manual CO2 calibration. Brightness and volume changes are saved back to `/config.json` so they survive restart.

The bottom row shows the five newest camera entries from `/cam`. Tapping an entry opens a full-screen JPEG preview using the CoreS3 display. The camera manager keeps up to 50 timestamped JPEG captures and removes the oldest files during rotation.

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
