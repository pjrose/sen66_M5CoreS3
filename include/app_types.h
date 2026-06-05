#pragma once

#include <Arduino.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

namespace aq {

constexpr uint8_t kSdaPin = 2;
constexpr uint8_t kSclPin = 1;
constexpr uint8_t kSen66Address = 0x6B;
constexpr uint8_t kLtr553Address = 0x23;

constexpr int kSdMiso = 35;
constexpr int kSdMosi = 37;
constexpr int kSdSck = 36;
constexpr int kSdCs = 4;

constexpr uint32_t kMeasurementIntervalMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kWarmupLeadMs = 2UL * 60UL * 1000UL;
constexpr uint32_t kIdleSleepAfterMs = 15UL * 60UL * 1000UL;

struct AppSettings {
    String wifiSsid;
    String wifiPassword;
    String mqttHost;
    uint16_t mqttPort = 1883;
    String mqttClientId = "m5stack-cores3-air";
    String mqttTopic = "air/station";
    uint16_t alarmCo2Ppm = 1200;
    float alarmPm25 = 35.0f;
    uint16_t alarmVoc = 250;
    uint8_t buzzerVolume = 96;
    uint8_t brightness = 170;
    uint16_t proximityThreshold = 150;
    bool ftpEnabled = false;
    String ftpUser = "air";
    String ftpPassword = "quality";
};

struct SensorSample {
    uint32_t timestamp = 0;
    float pm1p0 = NAN;
    float pm2p5 = NAN;
    float pm4p0 = NAN;
    float pm10p0 = NAN;
    float temperature = NAN;
    float humidity = NAN;
    float vocIndex = NAN;
    float noxIndex = NAN;
    uint16_t co2 = 0;
    bool valid = false;
};

struct __attribute__((packed)) LogRecord {
    uint32_t timestamp;
    uint16_t co2;
    uint16_t pm25_x10;
    uint16_t pm10_x10;
    int16_t temp_x100;
    uint16_t hum_x100;
    uint16_t voc_x10;
    uint16_t nox_x10;
    uint16_t pm1_x10;
    uint16_t pm4_x10;
    uint8_t version;
    uint8_t quality;
};

static_assert(sizeof(LogRecord) == 24, "LogRecord must remain 24 bytes");

struct __attribute__((packed)) HourMetric {
    int16_t mean;
    int16_t min;
    int16_t max;
};

struct __attribute__((packed)) HourSummaryRecord {
    uint32_t hourStart;
    HourMetric co2;
    HourMetric pm25_x10;
    HourMetric pm10_x10;
    HourMetric temp_x100;
    HourMetric hum_x100;
    HourMetric voc_x10;
    HourMetric nox_x10;
    uint16_t count;
};

static_assert(sizeof(HourSummaryRecord) == 48, "HourSummaryRecord must remain 48 bytes");

enum class Metric : uint8_t {
    Co2,
    Pm25,
    Pm10,
    Temperature,
    Humidity,
    Voc,
    Nox,
    Pm1,
    Pm4
};

enum class DeviceState : uint8_t {
    Active,
    IdleDimmed,
    LightSleep,
    Warmup,
    Measuring,
    Error
};

struct HistoryPoint {
    uint32_t timestamp = 0;
    int16_t value = 0;
};

inline uint16_t clampU16(float value, float scale) {
    if (!isfinite(value) || value < 0) {
        return 0xFFFF;
    }
    const float scaled = value * scale;
    if (scaled > 65534.0f) {
        return 65534;
    }
    return static_cast<uint16_t>(lroundf(scaled));
}

inline int16_t clampI16(float value, float scale) {
    if (!isfinite(value)) {
        return INT16_MAX;
    }
    const float scaled = value * scale;
    if (scaled > 32766.0f) {
        return 32766;
    }
    if (scaled < -32767.0f) {
        return -32767;
    }
    return static_cast<int16_t>(lroundf(scaled));
}

}  // namespace aq
