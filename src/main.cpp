#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <PubSubClient.h>
#include <SD.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <ESPmDNS.h>
#include <stdarg.h>
#include <time.h>

#include "audio_manager.h"
#include "camera_manager.h"
#include "config.h"
#include "ftp_manager.h"
#include "logger.h"
#include "sensor_manager.h"
#include "ui_manager.h"
#include "web_manager.h"

using namespace aq;

namespace {

constexpr int kProximityIntPin = -1;
constexpr uint32_t kStatusPaintMs = 1000;
constexpr uint32_t kChartRefreshMs = 30000;
constexpr uint32_t kNetworkRetryMs = 10000;
constexpr uint32_t kNtpRefreshMs = 24UL * 60UL * 60UL * 1000UL;
constexpr bool kRandomAlarmTestMode = false;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

SemaphoreHandle_t sampleMutex;
SemaphoreHandle_t mqttMutex;

SensorSample latestSample;
bool latestSampleReady = false;
bool alertActive = false;
bool previousAlertActive = false;
DeviceState state = DeviceState::Active;
String pendingMqttPayload;
uint32_t lastInteractionMs = 0;
uint32_t lastStatusPaintMs = 0;
uint32_t lastChartRefreshMs = 0;
uint32_t lastNtpSyncMs = 0;
uint32_t lastBaselineCheckMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastTouchLogMs = 0;
uint32_t lastTouchNavMs = 0;
uint32_t nextRandomAlarmTestMs = 0;
uint32_t randomAlarmTestCount = 0;
volatile uint32_t loopCounter = 0;
volatile bool sampleDirty = false;
volatile bool debugLineDirty = false;
bool displayDimmed = false;
bool wasPersonNearby = false;
uint32_t lastWalkupChirpMs = 0;
bool uiReady = false;
portMUX_TYPE debugLineMux = portMUX_INITIALIZER_UNLOCKED;
char latestDebugLine[160] = {0};

const char* stateText(DeviceState value) {
    switch (value) {
        case DeviceState::Active: return "ACTIVE";
        case DeviceState::IdleDimmed: return "DIM";
        case DeviceState::LightSleep: return "SLEEP";
        case DeviceState::Warmup: return "WARMUP";
        case DeviceState::Measuring: return "MEASURING";
        case DeviceState::Error: return "ERROR";
    }
    return "UNKNOWN";
}

const char* resetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

void logLine(const String& message) {
    Serial.printf("[%8lu] %s\r\n", millis(), message.c_str());
    Serial.flush();
    portENTER_CRITICAL(&debugLineMux);
    snprintf(latestDebugLine, sizeof(latestDebugLine), "%s", message.c_str());
    debugLineDirty = true;
    portEXIT_CRITICAL(&debugLineMux);
}

void logf(const char* format, ...) {
    char body[192] = {0};
    va_list args;
    va_start(args, format);
    vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    logLine(body);
}

void setState(DeviceState next) {
    if (state != next) {
        logf("State: %s -> %s", stateText(state), stateText(next));
    }
    state = next;
}

bool configureProximitySensor(const AppSettings& settings) {
    (void)settings;
    logLine("Proximity/ALS direct I2C disabled while validating CoreS3 touch bus");
    return false;
}

bool personDetected() {
    return false;
}

uint16_t ambientLightRaw() {
    return 100;
}

void applyTimezone() {
    const String tz = Config.settings().timezone.length() > 0 ? Config.settings().timezone : String("CST6CDT,M3.2.0,M11.1.0");
    setenv("TZ", tz.c_str(), 1);
    tzset();
}

bool sampleTriggersAlarm(const SensorSample& sample, bool currentlyActive) {
    if (!sample.valid) {
        return false;
    }
    const AppSettings& settings = Config.settings();
    const float hysteresis = constrain(settings.alarmHysteresisPercent, 0.0f, 50.0f) / 100.0f;
    const float clearFactor = currentlyActive ? 1.0f - hysteresis : 1.0f;
    return sample.co2 >= settings.alarmCo2Ppm * clearFactor ||
           sample.pm2p5 >= settings.alarmPm25 * clearFactor ||
           sample.vocIndex >= settings.alarmVoc * clearFactor;
}

String alertMessage(const SensorSample& sample) {
    if (sample.pm2p5 >= Config.settings().alarmPm25) {
        return "PM2.5 High (" + String(sample.pm2p5, 1) + " ug/m3)";
    }
    if (sample.co2 >= Config.settings().alarmCo2Ppm) {
        return "CO2 High (" + String(sample.co2) + " ppm)";
    }
    if (sample.vocIndex >= Config.settings().alarmVoc) {
        return "VOC High (" + String(sample.vocIndex, 0) + ")";
    }
    return "Air quality alarm";
}

String buildMqttPayload(const SensorSample& sample) {
    JsonDocument doc;
    doc["ts"] = sample.timestamp;
    doc["pm1_ugm3"] = sample.pm1p0;
    doc["pm25_ugm3"] = sample.pm2p5;
    doc["pm4_ugm3"] = sample.pm4p0;
    doc["pm10_ugm3"] = sample.pm10p0;
    doc["co2_ppm"] = sample.co2;
    doc["voc_index"] = sample.vocIndex;
    doc["nox_index"] = sample.noxIndex;
    doc["temp_c"] = sample.temperature;
    doc["humidity_pct"] = sample.humidity;
    String payload;
    serializeJson(doc, payload);
    return payload;
}

void queueMqttPayload(const String& payload) {
    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        pendingMqttPayload = payload;
        xSemaphoreGive(mqttMutex);
    }
}

String formatBytes(uint64_t bytes) {
    char buf[24] = {0};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return String(buf);
}

String formatEpoch(uint32_t epoch) {
    if (epoch == 0) {
        return "never";
    }
    time_t t = epoch;
    struct tm tmv {};
    localtime_r(&t, &tmv);
    char buf[24] = {0};
    strftime(buf, sizeof(buf), "%m/%d %H:%M", &tmv);
    return String(buf);
}

String maintenanceText() {
    const AppSettings& settings = Config.settings();
    const bool wifi = WiFi.status() == WL_CONNECTED;
    const uint64_t sdTotal = Config.sdReady() ? SD.totalBytes() : 0;
    const uint64_t sdUsed = Config.sdReady() ? SD.usedBytes() : 0;
    const uint8_t sdPct = sdTotal > 0 ? static_cast<uint8_t>((sdUsed * 100ULL) / sdTotal) : 0;

    String text;
    text.reserve(760);
    text += "Network\n";
    text += "Connected: " + String(wifi ? "yes" : "no") + "\n";
    text += "IP: " + (wifi ? WiFi.localIP().toString() : String("0.0.0.0")) + "\n";
    text += "Subnet: " + (wifi ? WiFi.subnetMask().toString() : String("0.0.0.0")) + "\n";
    text += "Gateway: " + (wifi ? WiFi.gatewayIP().toString() : String("0.0.0.0")) + "\n";
    text += "RSSI: " + (wifi ? String(WiFi.RSSI()) + " dBm" : String("--")) + "\n\n";

    text += "WiFi setup\n";
    text += "SSID: " + (settings.wifiSsid.length() ? settings.wifiSsid : String("(not set)")) + "\n";
    text += "Password: " + String(settings.wifiPassword.length() ? "set" : "not set") + "\n";
    text += "MQTT: " + (settings.mqttHost.length() ? settings.mqttHost + ":" + String(settings.mqttPort) : String("(off)")) + "\n";
    text += "TZ: " + settings.timezone + "\n\n";

    text += "Storage\n";
    text += "SD mounted: " + String(Config.sdReady() ? "yes" : "no") + "\n";
    text += "Used: " + formatBytes(sdUsed) + " / " + formatBytes(sdTotal) + " (" + String(sdPct) + "%)\n\n";

    text += "SEN66 health\n";
    text += "Online: " + String(Sensors.online() ? "yes" : "no") + "\n";
    text += "Measuring: " + String(Sensors.measuring() ? "yes" : "no") + "\n";
    text += "Warmup: " + String(Sensors.warming() ? "yes" : "no") + "\n";
    text += "Samples: " + String(Sensors.sampleSuccesses()) + "/" + String(Sensors.sampleAttempts()) + "\n";
    text += "Errors: " + String(Sensors.sampleErrors()) + "  I2C: " + String(Sensors.i2cErrors()) + "\n";
    text += "Last OK: " + formatEpoch(Sensors.lastSuccessEpoch()) + "\n";
    text += "Last err: " + formatEpoch(Sensors.lastErrorEpoch()) + "\n";
    text += "Detail: " + (Sensors.lastError().length() ? Sensors.lastError() : String("none")) + "\n\n";

    text += "Alarms\n";
    text += "CO2 " + String(settings.alarmCo2Ppm) + " ppm, PM2.5 " + String(settings.alarmPm25, 1) + ", VOC " + String(settings.alarmVoc) + "\n";
    text += "Hysteresis: " + String(settings.alarmHysteresisPercent, 1) + "%";
    return text;
}

bool updateLatestSample(const SensorSample& sample) {
    bool alertNow = false;
    if (xSemaphoreTake(sampleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        latestSample = sample;
        latestSampleReady = true;
        sampleDirty = true;
        alertActive = sampleTriggersAlarm(sample, alertActive);
        alertNow = alertActive;
        xSemaphoreGive(sampleMutex);
    }
    Web.updateSample(sample, alertNow, state);
    return alertNow;
}

void refreshChart() {
    HistoryPoint points[120];
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (Ui.filterChartMode()) {
        FilterDeviationSummary summary;
        const size_t count = Logger.readFilterDeviation(now, Config.settings(), points, 120, &summary);
        Ui.plotFilterDeviation(points, count, summary);
        return;
    }
    const size_t count = Ui.monthMode()
                             ? Logger.readMonth(now, Ui.selectedMetric(), points, 120, 120)
                             : Logger.readDay(now, Ui.selectedMetric(), points, 120, 120);
    if (count > 0) {
        Ui.plotHistory(points, count, Ui.selectedMetric());
    }
}

void startFilterBaselineCapture() {
    AppSettings& settings = Config.settings();
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    settings.filterBaselineActive = true;
    settings.filterBaselineReady = false;
    settings.filterBaselineStarted = now;
    settings.filterBaselineCompleted = 0;
    settings.filterBaselineSampleHours = 0;
    settings.filterBaselinePm25 = NAN;
    settings.filterBaselinePm10 = NAN;
    if (settings.filterBaselineHours < 24) {
        settings.filterBaselineHours = 72;
    }
    Config.save();
    Ui.updateBaselineStatus(settings);
    const String msg = "Filter baseline capture started (" + String(settings.filterBaselineHours) + "h)";
    Ui.addEvent(msg);
    Logger.appendAlert(msg, now);
}

void serviceFilterBaselineCapture() {
    const uint32_t nowMs = millis();
    if (nowMs - lastBaselineCheckMs < 60000UL) {
        return;
    }
    lastBaselineCheckMs = nowMs;

    AppSettings& settings = Config.settings();
    if (!settings.filterBaselineActive || settings.filterBaselineStarted == 0) {
        return;
    }

    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    const uint32_t requiredSeconds = static_cast<uint32_t>(settings.filterBaselineHours) * 3600UL;
    if (now < settings.filterBaselineStarted + requiredSeconds) {
        Ui.updateBaselineStatus(settings);
        return;
    }

    BaselineResult baseline;
    if (!Logger.computeParticulateBaseline(settings.filterBaselineStarted, now, baseline)) {
        Ui.updateBaselineStatus(settings);
        return;
    }

    settings.filterBaselineActive = false;
    settings.filterBaselineReady = true;
    settings.filterBaselineCompleted = now;
    settings.filterBaselinePm25 = baseline.pm25;
    settings.filterBaselinePm10 = baseline.pm10;
    settings.filterBaselineSampleHours = baseline.hours;
    Config.save();

    const String msg = "Filter baseline ready: PM2.5 " + String(baseline.pm25, 1) +
                       ", PM10 " + String(baseline.pm10, 1) +
                       " (" + String(baseline.hours) + "h)";
    Ui.addEvent(msg);
    Logger.appendAlert(msg, now);
    Ui.updateBaselineStatus(settings);
}

void captureCameraSnapshot(const char* reason) {
    String path;
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (Camera.capture(now, &path)) {
        const String msg = String("Camera saved ") + path;
        logLine(msg);
        Ui.addEvent(msg);
        Logger.appendAlert(msg, now);
        String recent[5];
        const size_t count = Camera.recentImages(recent, 5);
        Ui.updateCameraRoll(recent, count);
        return;
    }

    const String msg = String("Camera failed") + (reason ? String(" (") + reason + ")" : "") + ": " + Camera.lastError();
    logLine(msg);
    Ui.addEvent(msg);
    Logger.appendAlert(msg, now);
}

void serviceNetwork(void*) {
    uint32_t lastWifiAttempt = 0;
    uint32_t lastMqttAttempt = 0;
    bool mdnsStarted = false;
    bool lastWifiConnected = false;
    bool lastMqttConnected = false;

    logLine("Network task starting");
    WiFi.mode(WIFI_STA);
    logLine("WiFi mode set to STA");
    mqtt.setKeepAlive(15);

    for (;;) {
        const AppSettings& settings = Config.settings();
        const uint32_t nowMs = millis();

        if (settings.wifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED && nowMs - lastWifiAttempt > kNetworkRetryMs) {
            logf("WiFi connect attempt: ssid='%s'", settings.wifiSsid.c_str());
            WiFi.disconnect(false, false);
            WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
            lastWifiAttempt = nowMs;
        } else if (settings.wifiSsid.length() == 0 && lastWifiAttempt == 0) {
            logLine("WiFi SSID is empty; network features remain offline");
            lastWifiAttempt = nowMs;
        }

        if (WiFi.status() == WL_CONNECTED) {
            if (!lastWifiConnected) {
                logf("WiFi connected: ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
                lastWifiConnected = true;
            }
            if (!mdnsStarted && MDNS.begin("core-air")) {
                MDNS.addService("http", "tcp", 80);
                mdnsStarted = true;
                logLine("mDNS started: http://core-air.local/");
            }

            if (lastNtpSyncMs == 0 || nowMs - lastNtpSyncMs > kNtpRefreshMs) {
                applyTimezone();
                configTzTime(Config.settings().timezone.c_str(), "pool.ntp.org", "time.nist.gov");
                lastNtpSyncMs = nowMs;
                logf("NTP sync requested: tz=%s", Config.settings().timezone.c_str());
            }

            if (settings.mqttHost.length() > 0 && !mqtt.connected() && nowMs - lastMqttAttempt > kNetworkRetryMs) {
                mqtt.setServer(settings.mqttHost.c_str(), settings.mqttPort);
                logf("MQTT connect attempt: %s:%u", settings.mqttHost.c_str(), settings.mqttPort);
                const bool ok = mqtt.connect(settings.mqttClientId.c_str());
                logf("MQTT connect result: %s", ok ? "ok" : "failed");
                lastMqttAttempt = nowMs;
            }
            mqtt.loop();
            if (mqtt.connected() != lastMqttConnected) {
                lastMqttConnected = mqtt.connected();
                logf("MQTT state changed: %s", lastMqttConnected ? "connected" : "offline");
            }

            if (mqtt.connected() && xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (pendingMqttPayload.length() > 0) {
                    mqtt.publish(settings.mqttTopic.c_str(), pendingMqttPayload.c_str(), false);
                    logLine("MQTT payload published");
                    pendingMqttPayload.clear();
                }
                xSemaphoreGive(mqttMutex);
            }
        } else {
            if (lastWifiConnected) {
                logLine("WiFi disconnected");
                lastWifiConnected = false;
            }
            if (mdnsStarted) {
                MDNS.end();
                mdnsStarted = false;
                logLine("mDNS stopped");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void serviceAcquisition(void*) {
    uint32_t nextDueMs = millis() + kWarmupLeadMs;
    bool warmupStarted = false;

    logLine("Acquisition task starting");
    warmupStarted = Sensors.startWarmup(millis());
    logf("Initial SEN66 warmup start: %s", warmupStarted ? "ok" : Sensors.lastError().c_str());
    setState(warmupStarted ? DeviceState::Warmup : DeviceState::Error);

    for (;;) {
        const uint32_t nowMs = millis();
        Sensors.service(nowMs);

        const int32_t msUntilDue = static_cast<int32_t>(nextDueMs - nowMs);
        if (!warmupStarted && msUntilDue <= static_cast<int32_t>(kWarmupLeadMs)) {
            setState(DeviceState::Warmup);
            warmupStarted = Sensors.startWarmup(nowMs);
            logf("SEN66 warmup start: %s", warmupStarted ? "ok" : Sensors.lastError().c_str());
        }

        if (warmupStarted && Sensors.readyForMeasurement(nowMs) && static_cast<int32_t>(nowMs - nextDueMs) >= 0) {
            setState(DeviceState::Measuring);
            SensorSample sample;
            if (Sensors.readAveraged(5, 1000, sample)) {
                logf("Sample ok: PM2.5=%.1f CO2=%u VOC=%.0f", sample.pm2p5, sample.co2, sample.vocIndex);
                Logger.append(sample);
                const bool nowAlert = updateLatestSample(sample);
                queueMqttPayload(buildMqttPayload(sample));

                if (nowAlert && !previousAlertActive) {
                    const String msg = alertMessage(sample);
                    Logger.appendAlert(msg, sample.timestamp);
                    if (uiReady) {
                        Ui.addEvent(msg);
                    }
                }
                previousAlertActive = nowAlert;
            } else {
                const String msg = "Sensor Comm Error";
                logf("Sample failed: %s", Sensors.lastError().c_str());
                Logger.appendAlert(msg, static_cast<uint32_t>(time(nullptr)));
                if (uiReady) {
                    Ui.addEvent(msg);
                }
                setState(DeviceState::Error);
            }

            Sensors.standby();
            warmupStarted = false;
            nextDueMs += kMeasurementIntervalMs;
            if (static_cast<int32_t>(nowMs - nextDueMs) > 0) {
                nextDueMs = nowMs + kMeasurementIntervalMs;
            }
            setState(DeviceState::Active);
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void applyUiControls() {
    if (!uiReady) {
        return;
    }
    if (Ui.chartModeChanged()) {
        refreshChart();
        lastChartRefreshMs = millis();
    }
    uint8_t value = 0;
    if (Ui.brightnessChanged(value)) {
        Config.settings().brightness = value;
        M5.Display.setBrightness(value);
        Config.save();
    }
    if (Ui.volumeChanged(value)) {
        Config.settings().buzzerVolume = value;
        Audio.setVolume(value);
        Config.save();
    }
    bool ftpEnabled = false;
    if (Ui.ftpToggleChanged(ftpEnabled)) {
        Config.settings().ftpEnabled = ftpEnabled;
        Ftp.setEnabled(ftpEnabled, Config.settings());
        Config.save();
    }
    if (Ui.calibrationRequested()) {
        int16_t correction = 0;
        const bool ok = Sensors.performForcedCo2Calibration(420, correction);
        const String msg = ok ? "CO2 calibration correction " + String(correction) + " ppm" : "CO2 calibration failed";
        Ui.addEvent(msg);
        Logger.appendAlert(msg, static_cast<uint32_t>(time(nullptr)));
    }
    if (Ui.baselineResetRequested()) {
        startFilterBaselineCapture();
    }
    if (Ui.cameraSnapshotRequested()) {
        captureCameraSnapshot("manual");
    }
    if (Ui.silenceRequested()) {
        Audio.silence();
    }
}

void handleDisplayPower() {
    const bool touched = M5.Touch.getDetail().isPressed();
    const bool nearby = personDetected();
    const bool walkup = nearby && !wasPersonNearby;
    const bool wakeFromDim = touched && displayDimmed;
    wasPersonNearby = nearby;

    if ((walkup || wakeFromDim) && millis() - lastWalkupChirpMs > 30000UL) {
        Audio.chirp(alertActive);
        lastWalkupChirpMs = millis();
    }

    if (touched || nearby) {
        lastInteractionMs = millis();
        if (displayDimmed) {
            setCpuFrequencyMhz(240);
            M5.Display.wakeup();
            M5.Display.setBrightness(Config.settings().brightness);
            displayDimmed = false;
            delay(500);
            String path;
            if (Camera.capture(static_cast<uint32_t>(time(nullptr)), &path)) {
                String recent[5];
                const size_t count = Camera.recentImages(recent, 5);
                if (uiReady) {
                    Ui.updateCameraRoll(recent, count);
                }
            } else {
                logf("Camera wake capture failed: %s", Camera.lastError().c_str());
            }
        }
    }

    const bool darkAndEmpty = ambientLightRaw() < 5 && !nearby;
    if (!displayDimmed && darkAndEmpty && millis() - lastInteractionMs > kIdleSleepAfterMs) {
        M5.Display.setBrightness(0);
        M5.Display.sleep();
        setCpuFrequencyMhz(80);
        displayDimmed = true;
    }

    if (displayDimmed && !alertActive) {
        if (kProximityIntPin >= 0) {
            gpio_wakeup_enable(static_cast<gpio_num_t>(kProximityIntPin), GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
        }
        esp_sleep_enable_timer_wakeup(5ULL * 1000ULL * 1000ULL);
        esp_light_sleep_start();
    }
}

void serviceRawTouchDiagnostics() {
    const auto detail = M5.Touch.getDetail();
    const bool pressed = detail.isPressed();
    m5gfx::touch_point_t directTouch;
    const uint_fast8_t directCount = M5.Display.getTouch(&directTouch, 1);
    const bool directPressed = directCount > 0;
    const uint32_t nowMs = millis();
    if (pressed && nowMs - lastTouchLogMs > 500UL) {
        lastTouchLogMs = nowMs;
        logf("Touch raw: x=%d y=%d state=0x%02x", detail.x, detail.y, static_cast<unsigned int>(detail.state));
    }
    if (!pressed && directPressed && nowMs - lastTouchLogMs > 500UL) {
        lastTouchLogMs = nowMs;
        logf("Display touch direct: count=%u x=%d y=%d size=%u id=%u",
             static_cast<unsigned int>(directCount),
             directTouch.x,
             directTouch.y,
             static_cast<unsigned int>(directTouch.size),
             static_cast<unsigned int>(directTouch.id));
    }

    if ((pressed || directPressed) && nowMs - lastTouchNavMs > 900UL) {
        lastTouchNavMs = nowMs;
        const int x = pressed ? detail.x : directTouch.x;
        const int y = pressed ? detail.y : directTouch.y;
        if (uiReady && Ui.handleRawTouch(x, y)) {
            logf("Touch raw action: screen=%u x=%d y=%d", Ui.currentScreen(), x, y);
        }
    }
}

void serviceDebugLine() {
    if (!uiReady || !debugLineDirty) {
        return;
    }
    char line[sizeof(latestDebugLine)] = {0};
    portENTER_CRITICAL(&debugLineMux);
    snprintf(line, sizeof(line), "%s", latestDebugLine);
    debugLineDirty = false;
    portEXIT_CRITICAL(&debugLineMux);
    Ui.updateDebugMessage(String(line));
}

void paintStatusAndSample() {
    const uint32_t nowMs = millis();
    if (nowMs - lastStatusPaintMs < kStatusPaintMs) {
        return;
    }
    lastStatusPaintMs = nowMs;

    if (xSemaphoreTake(sampleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (latestSampleReady) {
            if (uiReady) {
                Ui.updateSample(latestSample);
                if (sampleDirty) {
                    logf("UI sample update: PM2.5=%.1f CO2=%u VOC=%.0f", latestSample.pm2p5, latestSample.co2, latestSample.vocIndex);
                    sampleDirty = false;
                }
            }
            Web.updateSample(latestSample, alertActive, state);
        }
        Audio.setAlarmActive(alertActive);
        Audio.resetMuteIfClear(alertActive);
        if (uiReady) {
            Ui.updateAlarm(alertActive, Audio.muted());
        }
        xSemaphoreGive(sampleMutex);
    }

    if (uiReady) {
        Ui.updateStatus(WiFi.status() == WL_CONNECTED, mqtt.connected(), Config.sdReady(), state);
        Ui.updateBaselineStatus(Config.settings());
        Ui.updateMaintenance(maintenanceText());
    }
}

void serviceRandomAlarmTest() {
    if (!kRandomAlarmTestMode || !uiReady) {
        return;
    }
    const uint32_t nowMs = millis();
    if (nextRandomAlarmTestMs == 0) {
        nextRandomAlarmTestMs = nowMs + static_cast<uint32_t>(random(1000, 10001));
        logLine("TEST MODE: random alarms enabled every 1-10 seconds");
        return;
    }
    if (static_cast<int32_t>(nowMs - nextRandomAlarmTestMs) < 0) {
        return;
    }

    randomAlarmTestCount++;
    const String msg = "TEST Random Alarm #" + String(randomAlarmTestCount);
    Ui.addEvent(msg);
    Logger.appendAlert(msg, static_cast<uint32_t>(time(nullptr)));
    Audio.chirp(true);
    logLine(msg);
    nextRandomAlarmTestMs = nowMs + static_cast<uint32_t>(random(1000, 10001));
}

void serviceWeb(void*) {
    logLine("Web task starting");
    for (;;) {
        Web.updateNetwork(WiFi.status() == WL_CONNECTED, mqtt.connected());
        Web.service();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void serviceSerialDiagnostics(void*) {
    logLine("Serial diagnostics task starting");
    uint32_t lastLoopCounter = 0;
    uint32_t lastReportMs = 0;
    for (;;) {
        const uint32_t nowMs = millis();
        if (nowMs - lastReportMs >= 5000UL) {
            lastReportMs = nowMs;
            const String wifiText = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("offline");
            const uint32_t loops = loopCounter;
            logf("Heartbeat: state=%s ui=%s sd=%s wifi=%s mqtt=%s heap=%u loops=%lu delta=%lu",
                 stateText(state),
                 uiReady ? "ok" : "off",
                 Config.sdReady() ? "ok" : "missing",
                 wifiText.c_str(),
                 mqtt.connected() ? "connected" : "offline",
                 ESP.getFreeHeap(),
                 static_cast<unsigned long>(loops),
                 static_cast<unsigned long>(loops - lastLoopCounter));
            lastLoopCounter = loops;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void serviceSerialHeartbeat() {
    const uint32_t nowMs = millis();
    if (nowMs - lastHeartbeatMs < 5000UL) {
        return;
    }
    lastHeartbeatMs = nowMs;
    const String wifiText = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("offline");
    logf("Heartbeat: state=%s ui=%s sd=%s wifi=%s mqtt=%s heap=%u",
         stateText(state),
         uiReady ? "ok" : "off",
         Config.sdReady() ? "ok" : "missing",
         wifiText.c_str(),
         mqtt.connected() ? "connected" : "offline",
         ESP.getFreeHeap());
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    const uint32_t serialWaitStarted = millis();
    while (!Serial && millis() - serialWaitStarted < 3000UL) {
        delay(10);
    }
    delay(200);
    randomSeed(esp_random());
    logLine("");
    logLine("========================================");
    logLine("CoreS3 Air Station boot");
    logf("Reset reason: %s (%d)", resetReasonText(esp_reset_reason()), static_cast<int>(esp_reset_reason()));
    logf("Build: %s %s", __DATE__, __TIME__);
    logf("Free heap before M5.begin: %u", ESP.getFreeHeap());

    auto cfg = M5.config();
    logLine("M5.begin starting");
    M5.begin(cfg);
    logLine("M5.begin ok");
    M5.Display.setRotation(1);
    M5.Display.setBrightness(160);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("CoreS3 Air Station", 12, 12);
    M5.Display.drawString("Booting...", 12, 34);
    logf("Display ready: %dx%d rotation=%d", M5.Display.width(), M5.Display.height(), M5.Display.getRotation());

    logLine("Creating mutexes");
    sampleMutex = xSemaphoreCreateMutex();
    mqttMutex = xSemaphoreCreateMutex();
    logf("Mutexes: sample=%s mqtt=%s", sampleMutex ? "ok" : "failed", mqttMutex ? "ok" : "failed");

    logLine("Config.begin starting");
    const bool configOk = Config.begin();
    logf("Config.begin result: ok=%s sd=%s err='%s'", configOk ? "true" : "false", Config.sdReady() ? "true" : "false", Config.lastError().c_str());
    applyTimezone();
    logf("Timezone applied: %s", Config.settings().timezone.c_str());
    logLine("Logger.begin starting");
    const bool loggerOk = Logger.begin();
    logf("Logger.begin result: %s err='%s'", loggerOk ? "ok" : "failed", Logger.lastError().c_str());
    logLine("Audio.begin starting");
    Audio.begin(Config.settings().buzzerVolume);
    logLine("Audio.begin ok");
    logLine("Ui.begin starting");
    uiReady = Ui.begin(Config.settings());
    logf("Ui.begin result: %s", uiReady ? "ok" : "failed");
    logLine("Ftp.begin starting");
    Ftp.begin(Config.settings());
    logLine("Ftp.begin ok");
    logLine("Web.begin starting");
    Web.begin();
    logLine("Web.begin ok; HTTP listener starts after WiFi connects");
    logLine("Proximity/ALS init starting");
    const bool proxOk = configureProximitySensor(Config.settings());
    logf("Proximity/ALS init result: %s", proxOk ? "ok" : "not found");
    logLine("SEN66 begin starting");
    const bool sensorOk = Sensors.begin(Wire);
    logf("SEN66 begin result: %s err='%s'", sensorOk ? "ok" : "failed", Sensors.lastError().c_str());
    String recent[5];
    const size_t recentCount = Config.sdReady() ? Camera.recentImages(recent, 5) : 0;
    logf("Recent camera images listed: %u", static_cast<unsigned int>(recentCount));
    if (uiReady) {
        Ui.updateCameraRoll(recent, recentCount);
    }
    if (uiReady) {
        Ui.updateBaselineStatus(Config.settings());
    }

    lastInteractionMs = millis();
    logLine("Starting FreeRTOS tasks");
    xTaskCreatePinnedToCore(serviceSerialDiagnostics, "serial_diag", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(serviceWeb, "web", 6144, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(serviceNetwork, "network", 8192, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(serviceAcquisition, "acquisition", 8192, nullptr, 1, nullptr, 0);
    logLine("Setup complete");
}

void loop() {
    M5.update();
    serviceRawTouchDiagnostics();
    serviceDebugLine();
    if (uiReady) {
        Ui.tick();
    }
    Ftp.service();
    Audio.service(millis());
    applyUiControls();
    if (uiReady) {
        serviceFilterBaselineCapture();
    }
    paintStatusAndSample();
    serviceRandomAlarmTest();
    loopCounter++;

    if (uiReady && millis() - lastChartRefreshMs > kChartRefreshMs) {
        lastChartRefreshMs = millis();
        refreshChart();
    }

    handleDisplayPower();
    delay(5);
}
