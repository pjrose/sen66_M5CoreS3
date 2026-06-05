#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <time.h>

#include "audio_manager.h"
#include "camera_manager.h"
#include "config.h"
#include "ftp_manager.h"
#include "logger.h"
#include "sensor_manager.h"
#include "ui_manager.h"

using namespace aq;

namespace {

constexpr uint8_t kInternalSda = 12;
constexpr uint8_t kInternalScl = 11;
constexpr int kProximityIntPin = -1;
constexpr uint32_t kStatusPaintMs = 1000;
constexpr uint32_t kChartRefreshMs = 30000;
constexpr uint32_t kNetworkRetryMs = 10000;
constexpr uint32_t kNtpRefreshMs = 24UL * 60UL * 60UL * 1000UL;

TwoWire InternalI2C(1);
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
bool displayDimmed = false;

void setState(DeviceState next) {
    state = next;
}

bool writeLtrRegister(uint8_t reg, uint8_t value) {
    InternalI2C.beginTransmission(kLtr553Address);
    InternalI2C.write(reg);
    InternalI2C.write(value);
    return InternalI2C.endTransmission() == 0;
}

uint16_t readLtrWord(uint8_t regLow) {
    InternalI2C.beginTransmission(kLtr553Address);
    InternalI2C.write(regLow);
    if (InternalI2C.endTransmission(false) != 0) {
        return 0;
    }
    if (InternalI2C.requestFrom(kLtr553Address, static_cast<uint8_t>(2)) != 2) {
        return 0;
    }
    const uint8_t lo = InternalI2C.read();
    const uint8_t hi = InternalI2C.read();
    return static_cast<uint16_t>(hi << 8 | lo);
}

bool configureProximitySensor(const AppSettings& settings) {
    InternalI2C.begin(kInternalSda, kInternalScl, 100000);
    InternalI2C.setTimeOut(20);
    InternalI2C.beginTransmission(kLtr553Address);
    if (InternalI2C.endTransmission() != 0) {
        return false;
    }

    writeLtrRegister(0x80, 0x03);
    writeLtrRegister(0x81, 0x03);
    writeLtrRegister(0x84, 0x03);
    writeLtrRegister(0x85, 0x12);
    writeLtrRegister(0x8F, 0x01);

    const uint16_t threshold = settings.proximityThreshold;
    writeLtrRegister(0x90, threshold & 0xFF);
    writeLtrRegister(0x91, threshold >> 8);
    writeLtrRegister(0x92, 0x00);
    writeLtrRegister(0x93, 0x00);
    return true;
}

bool personDetected() {
    return readLtrWord(0x8D) >= Config.settings().proximityThreshold;
}

uint16_t ambientLightRaw() {
    return readLtrWord(0x88);
}

bool sampleTriggersAlarm(const SensorSample& sample) {
    return sample.valid &&
           (sample.co2 >= Config.settings().alarmCo2Ppm ||
            sample.pm2p5 >= Config.settings().alarmPm25 ||
            sample.vocIndex >= Config.settings().alarmVoc);
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

void updateLatestSample(const SensorSample& sample) {
    if (xSemaphoreTake(sampleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        latestSample = sample;
        latestSampleReady = true;
        alertActive = sampleTriggersAlarm(sample);
        xSemaphoreGive(sampleMutex);
    }
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

void serviceNetwork(void*) {
    uint32_t lastWifiAttempt = 0;
    uint32_t lastMqttAttempt = 0;

    WiFi.mode(WIFI_STA);
    mqtt.setKeepAlive(15);

    for (;;) {
        const AppSettings& settings = Config.settings();
        const uint32_t nowMs = millis();

        if (settings.wifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED && nowMs - lastWifiAttempt > kNetworkRetryMs) {
            WiFi.disconnect(false, false);
            WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
            lastWifiAttempt = nowMs;
        }

        if (WiFi.status() == WL_CONNECTED) {
            if (lastNtpSyncMs == 0 || nowMs - lastNtpSyncMs > kNtpRefreshMs) {
                configTime(0, 0, "pool.ntp.org", "time.nist.gov");
                lastNtpSyncMs = nowMs;
            }

            if (settings.mqttHost.length() > 0 && !mqtt.connected() && nowMs - lastMqttAttempt > kNetworkRetryMs) {
                mqtt.setServer(settings.mqttHost.c_str(), settings.mqttPort);
                mqtt.connect(settings.mqttClientId.c_str());
                lastMqttAttempt = nowMs;
            }
            mqtt.loop();

            if (mqtt.connected() && xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (pendingMqttPayload.length() > 0) {
                    mqtt.publish(settings.mqttTopic.c_str(), pendingMqttPayload.c_str(), false);
                    pendingMqttPayload.clear();
                }
                xSemaphoreGive(mqttMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void serviceAcquisition(void*) {
    uint32_t nextDueMs = millis() + kWarmupLeadMs;
    bool warmupStarted = false;

    Sensors.startWarmup(millis());
    warmupStarted = true;
    setState(DeviceState::Warmup);

    for (;;) {
        const uint32_t nowMs = millis();
        Sensors.service(nowMs);

        const int32_t msUntilDue = static_cast<int32_t>(nextDueMs - nowMs);
        if (!warmupStarted && msUntilDue <= static_cast<int32_t>(kWarmupLeadMs)) {
            setState(DeviceState::Warmup);
            warmupStarted = Sensors.startWarmup(nowMs);
        }

        if (warmupStarted && Sensors.readyForMeasurement(nowMs) && static_cast<int32_t>(nowMs - nextDueMs) >= 0) {
            setState(DeviceState::Measuring);
            SensorSample sample;
            if (Sensors.readAveraged(5, 1000, sample)) {
                Logger.append(sample);
                updateLatestSample(sample);
                queueMqttPayload(buildMqttPayload(sample));

                const bool nowAlert = sampleTriggersAlarm(sample);
                if (nowAlert && !previousAlertActive) {
                    const String msg = alertMessage(sample);
                    Logger.appendAlert(msg, sample.timestamp);
                    Ui.addEvent(msg);
                }
                previousAlertActive = nowAlert;
            } else {
                const String msg = "Sensor Comm Error";
                Logger.appendAlert(msg, static_cast<uint32_t>(time(nullptr)));
                Ui.addEvent(msg);
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
    if (Ui.silenceRequested()) {
        Audio.silence();
    }
}

void handleDisplayPower() {
    const bool touched = M5.Touch.getDetail().isPressed();
    if (touched || personDetected()) {
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
                Ui.updateCameraRoll(recent, count);
            }
        }
    }

    const bool darkAndEmpty = ambientLightRaw() < 5 && !personDetected();
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

void paintStatusAndSample() {
    const uint32_t nowMs = millis();
    if (nowMs - lastStatusPaintMs < kStatusPaintMs) {
        return;
    }
    lastStatusPaintMs = nowMs;

    if (xSemaphoreTake(sampleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (latestSampleReady) {
            Ui.updateSample(latestSample);
        }
        Audio.setAlarmActive(alertActive);
        Audio.resetMuteIfClear(alertActive);
        Ui.updateAlarm(alertActive, Audio.muted());
        xSemaphoreGive(sampleMutex);
    }

    Ui.updateStatus(WiFi.status() == WL_CONNECTED, mqtt.connected(), Config.sdReady(), state);
    Ui.updateBaselineStatus(Config.settings());
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);

    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(160);

    sampleMutex = xSemaphoreCreateMutex();
    mqttMutex = xSemaphoreCreateMutex();

    Config.begin();
    Logger.begin();
    Audio.begin(Config.settings().buzzerVolume);
    Ui.begin(Config.settings());
    Ftp.begin(Config.settings());
    configureProximitySensor(Config.settings());
    Sensors.begin(Wire);
    Camera.begin();
    String recent[5];
    const size_t recentCount = Camera.recentImages(recent, 5);
    Ui.updateCameraRoll(recent, recentCount);
    Ui.updateBaselineStatus(Config.settings());

    lastInteractionMs = millis();
    xTaskCreatePinnedToCore(serviceNetwork, "network", 8192, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(serviceAcquisition, "acquisition", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
    M5.update();
    Ui.tick();
    Ftp.service();
    Audio.service(millis());
    applyUiControls();
    serviceFilterBaselineCapture();
    paintStatusAndSample();

    if (millis() - lastChartRefreshMs > kChartRefreshMs) {
        lastChartRefreshMs = millis();
        refreshChart();
    }

    handleDisplayPower();
    delay(5);
}
