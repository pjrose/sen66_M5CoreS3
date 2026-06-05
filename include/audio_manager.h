#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include "app_types.h"

namespace aq {

class AudioManager {
public:
    void begin(uint8_t volume);
    void setVolume(uint8_t volume);
    void setAlarmActive(bool active);
    void chirp(bool urgent = false);
    void silence();
    void resetMuteIfClear(bool alertActive);
    void service(uint32_t nowMs);
    bool muted() const { return muted_; }
    bool alarmActive() const { return alarmActive_; }

private:
    uint8_t volume_ = 96;
    bool alarmActive_ = false;
    bool muted_ = false;
    bool chirpActive_ = false;
    bool chirpUrgent_ = false;
    uint32_t lastToneMs_ = 0;
    uint8_t toneStep_ = 0;
};

extern AudioManager Audio;

}  // namespace aq
