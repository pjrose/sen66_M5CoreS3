#include "config.h"

#include <ArduinoJson.h>
#include <SPI.h>

namespace aq {

ConfigManager Config;

namespace {
constexpr const char* kConfigPath = "/config.json";
}

bool ConfigManager::begin() {
    applyDefaults();
    sdReady_ = mountSd();
    if (!sdReady_) {
        return false;
    }
    ensureDataDirectories();
    if (!SD.exists(kConfigPath)) {
        return save();
    }
    return load();
}

bool ConfigManager::mountSd() {
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    if (SD.begin(kSdCs, SPI, 25000000)) {
        return true;
    }
    if (SD.begin()) {
        return true;
    }
    lastError_ = "SD mount failed";
    return false;
}

bool ConfigManager::ensureDataDirectories() const {
    if (!sdReady_) {
        return false;
    }
    bool ok = true;
    if (!SD.exists("/log")) {
        ok &= SD.mkdir("/log");
    }
    if (!SD.exists("/cam")) {
        ok &= SD.mkdir("/cam");
    }
    return ok;
}

void ConfigManager::applyDefaults() {
    settings_ = AppSettings{};
}

bool ConfigManager::load() {
    if (!sdReady_) {
        lastError_ = "SD unavailable";
        return false;
    }
    File file = SD.open(kConfigPath, FILE_READ);
    if (!file) {
        lastError_ = "Unable to open config";
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        lastError_ = String("Config parse failed: ") + err.c_str();
        save();
        return false;
    }

    settings_.wifiSsid = doc["wifi_ssid"] | settings_.wifiSsid;
    settings_.wifiPassword = doc["wifi_password"] | settings_.wifiPassword;
    settings_.mqttHost = doc["mqtt_host"] | settings_.mqttHost;
    settings_.mqttPort = doc["mqtt_port"] | settings_.mqttPort;
    settings_.mqttClientId = doc["mqtt_client_id"] | settings_.mqttClientId;
    settings_.mqttTopic = doc["mqtt_topic"] | settings_.mqttTopic;
    settings_.alarmCo2Ppm = doc["alarm_co2_ppm"] | settings_.alarmCo2Ppm;
    settings_.alarmPm25 = doc["alarm_pm25_ugm3"] | settings_.alarmPm25;
    settings_.alarmVoc = doc["alarm_voc_index"] | settings_.alarmVoc;
    settings_.alarmHysteresisPercent = doc["alarm_hysteresis_percent"] | settings_.alarmHysteresisPercent;
    settings_.timezone = doc["timezone"] | settings_.timezone;
    settings_.buzzerVolume = doc["buzzer_volume"] | settings_.buzzerVolume;
    settings_.brightness = doc["brightness"] | settings_.brightness;
    settings_.proximityThreshold = doc["proximity_threshold"] | settings_.proximityThreshold;
    settings_.ftpEnabled = doc["ftp_enabled"] | settings_.ftpEnabled;
    settings_.ftpUser = doc["ftp_user"] | settings_.ftpUser;
    settings_.ftpPassword = doc["ftp_password"] | settings_.ftpPassword;

    JsonObject filter = doc["filter_baseline"];
    if (!filter.isNull()) {
        settings_.filterBaselineActive = filter["active"] | settings_.filterBaselineActive;
        settings_.filterBaselineReady = filter["ready"] | settings_.filterBaselineReady;
        settings_.filterBaselineStarted = filter["started_epoch"] | settings_.filterBaselineStarted;
        settings_.filterBaselineCompleted = filter["completed_epoch"] | settings_.filterBaselineCompleted;
        settings_.filterBaselineHours = filter["capture_hours"] | settings_.filterBaselineHours;
        settings_.filterBaselinePm25 = filter["pm25_ugm3"] | settings_.filterBaselinePm25;
        settings_.filterBaselinePm10 = filter["pm10_ugm3"] | settings_.filterBaselinePm10;
        settings_.filterBaselineSampleHours = filter["sample_hours"] | settings_.filterBaselineSampleHours;
        settings_.filterWarnPercent = filter["warn_percent"] | settings_.filterWarnPercent;
        settings_.filterReplacePercent = filter["replace_percent"] | settings_.filterReplacePercent;
    }
    return true;
}

bool ConfigManager::save() const {
    if (!sdReady_) {
        return false;
    }

    JsonDocument doc;
    doc["wifi_ssid"] = settings_.wifiSsid;
    doc["wifi_password"] = settings_.wifiPassword;
    doc["mqtt_host"] = settings_.mqttHost;
    doc["mqtt_port"] = settings_.mqttPort;
    doc["mqtt_client_id"] = settings_.mqttClientId;
    doc["mqtt_topic"] = settings_.mqttTopic;
    doc["alarm_co2_ppm"] = settings_.alarmCo2Ppm;
    doc["alarm_pm25_ugm3"] = settings_.alarmPm25;
    doc["alarm_voc_index"] = settings_.alarmVoc;
    doc["alarm_hysteresis_percent"] = settings_.alarmHysteresisPercent;
    doc["timezone"] = settings_.timezone;
    doc["buzzer_volume"] = settings_.buzzerVolume;
    doc["brightness"] = settings_.brightness;
    doc["proximity_threshold"] = settings_.proximityThreshold;
    doc["ftp_enabled"] = settings_.ftpEnabled;
    doc["ftp_user"] = settings_.ftpUser;
    doc["ftp_password"] = settings_.ftpPassword;

    JsonObject filter = doc["filter_baseline"].to<JsonObject>();
    filter["active"] = settings_.filterBaselineActive;
    filter["ready"] = settings_.filterBaselineReady;
    filter["started_epoch"] = settings_.filterBaselineStarted;
    filter["completed_epoch"] = settings_.filterBaselineCompleted;
    filter["capture_hours"] = settings_.filterBaselineHours;
    filter["pm25_ugm3"] = settings_.filterBaselinePm25;
    filter["pm10_ugm3"] = settings_.filterBaselinePm10;
    filter["sample_hours"] = settings_.filterBaselineSampleHours;
    filter["warn_percent"] = settings_.filterWarnPercent;
    filter["replace_percent"] = settings_.filterReplacePercent;

    SD.remove(kConfigPath);
    File file = SD.open(kConfigPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const bool ok = serializeJsonPretty(doc, file) > 0;
    file.close();
    return ok;
}

}  // namespace aq
