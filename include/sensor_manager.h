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
    String lastError() const { return lastError_; }
    uint32_t warmupStartedMs() const { return warmupStartedMs_; }

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
    String lastError_;
};

extern SensorManager Sensors;

}  // namespace aq

