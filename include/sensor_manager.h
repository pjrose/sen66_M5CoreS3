#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cSen66.h>
#include "app_types.h"

namespace aq {

class SensorManager {
public:
    bool begin(TwoWire& wire = Wire);
    bool startWarmup(uint32_t nowMs);
    bool standby();
    void service(uint32_t nowMs);
    bool readyForMeasurement(uint32_t nowMs) const;
    bool readSample(SensorSample& out);
    bool readAveraged(uint8_t samples, uint32_t perSampleTimeoutMs, SensorSample& out);
    bool performForcedCo2Calibration(uint16_t targetPpm, int16_t& correction);

    bool online() const { return online_; }
    bool warming() const { return warming_; }
    bool measuring() const { return measuring_; }
    String lastError() const { return lastError_; }
    uint32_t warmupStartedMs() const { return warmupStartedMs_; }
    uint32_t sampleAttempts() const { return sampleAttempts_; }
    uint32_t sampleSuccesses() const { return sampleSuccesses_; }
    uint32_t sampleErrors() const { return sampleErrors_; }
    uint32_t i2cErrors() const { return i2cErrors_; }
    uint32_t lastSuccessEpoch() const { return lastSuccessEpoch_; }
    uint32_t lastErrorEpoch() const { return lastErrorEpoch_; }

private:
    bool scanAddress(uint8_t address);
    bool validRaw(uint16_t value) const { return value != 0xFFFF; }
    bool validRaw(int16_t value) const { return value != INT16_MAX; }
    void setError(int16_t errorCode, const char* operation);

    SensirionI2cSen66 sensor_;
    TwoWire* wire_ = nullptr;
    bool online_ = false;
    bool measuring_ = false;
    bool warming_ = false;
    uint32_t warmupStartedMs_ = 0;
    uint32_t sampleAttempts_ = 0;
    uint32_t sampleSuccesses_ = 0;
    uint32_t sampleErrors_ = 0;
    uint32_t i2cErrors_ = 0;
    uint32_t lastSuccessEpoch_ = 0;
    uint32_t lastErrorEpoch_ = 0;
    String lastError_;
};

extern SensorManager Sensors;

}  // namespace aq
