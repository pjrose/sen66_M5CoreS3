#include "sensor_manager.h"

#include <SensirionCore.h>
#include <time.h>

namespace aq {

SensorManager Sensors;

namespace {
constexpr int16_t kNoError = 0;
constexpr uint32_t kWarmupMs = kWarmupLeadMs;
}

bool SensorManager::begin(TwoWire& wire) {
    wire_ = &wire;
    wire_->begin(kSdaPin, kSclPin, 100000);
    wire_->setTimeOut(50);

    if (!scanAddress(kSen66Address)) {
        lastError_ = "SEN66 not found at 0x6B";
        online_ = false;
        i2cErrors_++;
        lastErrorEpoch_ = static_cast<uint32_t>(time(nullptr));
        return false;
    }

    sensor_.begin(*wire_, SEN66_I2C_ADDR_6B);
    const int16_t resetErr = sensor_.deviceReset();
    if (resetErr != kNoError) {
        setError(resetErr, "deviceReset");
        online_ = false;
        return false;
    }

    delay(1200);
    online_ = true;
    measuring_ = false;
    warming_ = false;
    lastError_.clear();
    return true;
}

bool SensorManager::scanAddress(uint8_t address) {
    wire_->beginTransmission(address);
    return wire_->endTransmission() == 0;
}

bool SensorManager::startWarmup(uint32_t nowMs) {
    if (!online_ && !begin(*wire_)) {
        return false;
    }
    if (measuring_) {
        warming_ = true;
        warmupStartedMs_ = nowMs;
        return true;
    }
    const int16_t err = sensor_.startContinuousMeasurement();
    if (err != kNoError) {
        setError(err, "startContinuousMeasurement");
        measuring_ = false;
        warming_ = false;
        return false;
    }
    measuring_ = true;
    warming_ = true;
    warmupStartedMs_ = nowMs;
    return true;
}

bool SensorManager::standby() {
    if (!online_ || !measuring_) {
        measuring_ = false;
        warming_ = false;
        return true;
    }
    const int16_t err = sensor_.stopMeasurement();
    delay(1000);
    measuring_ = false;
    warming_ = false;
    if (err != kNoError) {
        setError(err, "stopMeasurement");
        return false;
    }
    return true;
}

void SensorManager::service(uint32_t nowMs) {
    if (warming_ && nowMs - warmupStartedMs_ >= kWarmupMs) {
        warming_ = false;
    }
}

bool SensorManager::readyForMeasurement(uint32_t nowMs) const {
    return online_ && measuring_ && nowMs - warmupStartedMs_ >= kWarmupMs;
}

bool SensorManager::readSample(SensorSample& out) {
    sampleAttempts_++;
    if (!online_ || !measuring_) {
        lastError_ = "SEN66 is not measuring";
        sampleErrors_++;
        lastErrorEpoch_ = static_cast<uint32_t>(time(nullptr));
        return false;
    }

    uint16_t pm1 = 0;
    uint16_t pm25 = 0;
    uint16_t pm4 = 0;
    uint16_t pm10 = 0;
    int16_t hum = 0;
    int16_t temp = 0;
    int16_t voc = 0;
    int16_t nox = 0;
    uint16_t co2 = 0;

    const int16_t err = sensor_.readMeasuredValuesAsIntegers(pm1, pm25, pm4, pm10, hum, temp, voc, nox, co2);
    if (err != kNoError) {
        setError(err, "readMeasuredValuesAsIntegers");
        return false;
    }

    out.timestamp = static_cast<uint32_t>(time(nullptr));
    out.pm1p0 = validRaw(pm1) ? pm1 / 10.0f : NAN;
    out.pm2p5 = validRaw(pm25) ? pm25 / 10.0f : NAN;
    out.pm4p0 = validRaw(pm4) ? pm4 / 10.0f : NAN;
    out.pm10p0 = validRaw(pm10) ? pm10 / 10.0f : NAN;
    out.humidity = validRaw(hum) ? hum / 100.0f : NAN;
    out.temperature = validRaw(temp) ? temp / 200.0f : NAN;
    out.vocIndex = validRaw(voc) ? voc / 10.0f : NAN;
    out.noxIndex = validRaw(nox) ? nox / 10.0f : NAN;
    out.co2 = validRaw(co2) ? co2 : 0;
    out.valid = isfinite(out.pm2p5) || out.co2 > 0 || isfinite(out.temperature);
    if (out.valid) {
        sampleSuccesses_++;
        lastSuccessEpoch_ = out.timestamp;
    } else {
        sampleErrors_++;
        lastErrorEpoch_ = out.timestamp;
    }
    return out.valid;
}

bool SensorManager::readAveraged(uint8_t samples, uint32_t perSampleTimeoutMs, SensorSample& out) {
    if (samples == 0) {
        return false;
    }

    SensorSample acc;
    uint8_t accepted = 0;
    const uint32_t started = millis();

    while (accepted < samples && millis() - started < perSampleTimeoutMs * samples + 1000) {
        SensorSample current;
        if (readSample(current)) {
            auto addFinite = [](float& dst, float src) {
                if (isfinite(src)) {
                    if (!isfinite(dst)) {
                        dst = 0;
                    }
                    dst += src;
                }
            };
            addFinite(acc.pm1p0, current.pm1p0);
            addFinite(acc.pm2p5, current.pm2p5);
            addFinite(acc.pm4p0, current.pm4p0);
            addFinite(acc.pm10p0, current.pm10p0);
            addFinite(acc.temperature, current.temperature);
            addFinite(acc.humidity, current.humidity);
            addFinite(acc.vocIndex, current.vocIndex);
            addFinite(acc.noxIndex, current.noxIndex);
            acc.co2 += current.co2;
            accepted++;
        }
        delay(perSampleTimeoutMs);
    }

    if (accepted == 0) {
        lastError_ = "No valid samples in burst";
        sampleErrors_++;
        lastErrorEpoch_ = static_cast<uint32_t>(time(nullptr));
        return false;
    }

    auto divFinite = [accepted](float& value) {
        if (isfinite(value)) {
            value /= accepted;
        }
    };
    divFinite(acc.pm1p0);
    divFinite(acc.pm2p5);
    divFinite(acc.pm4p0);
    divFinite(acc.pm10p0);
    divFinite(acc.temperature);
    divFinite(acc.humidity);
    divFinite(acc.vocIndex);
    divFinite(acc.noxIndex);
    acc.co2 = acc.co2 / accepted;
    acc.timestamp = static_cast<uint32_t>(time(nullptr));
    acc.valid = true;
    out = acc;
    return true;
}

bool SensorManager::performForcedCo2Calibration(uint16_t targetPpm, int16_t& correction) {
    if (!online_) {
        return false;
    }
    standby();
    delay(600);
    uint16_t rawCorrection = 0;
    const int16_t err = sensor_.performForcedCo2Recalibration(targetPpm, rawCorrection);
    if (err != kNoError || rawCorrection == 0xFFFF) {
        setError(err, "performForcedCo2Recalibration");
        return false;
    }
    correction = static_cast<int16_t>(rawCorrection) - static_cast<int16_t>(0x8000);
    return true;
}

void SensorManager::setError(int16_t errorCode, const char* operation) {
    char message[80] = {0};
    errorToString(errorCode, message, sizeof(message));
    lastError_ = String(operation) + ": " + message;
    online_ = false;
    sampleErrors_++;
    i2cErrors_++;
    lastErrorEpoch_ = static_cast<uint32_t>(time(nullptr));
}

}  // namespace aq
