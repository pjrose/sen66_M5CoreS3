#include "audio_manager.h"

namespace aq {

AudioManager Audio;

void AudioManager::begin(uint8_t volume) {
    volume_ = volume;
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume_);
}

void AudioManager::setVolume(uint8_t volume) {
    volume_ = volume;
    M5.Speaker.setVolume(volume_);
}

void AudioManager::setAlarmActive(bool active) {
    alarmActive_ = active;
    if (!active) {
        muted_ = false;
    }
}

void AudioManager::chirp(bool urgent) {
    if (muted_) {
        return;
    }
    chirpActive_ = true;
    chirpUrgent_ = urgent;
    toneStep_ = 0;
    lastToneMs_ = 0;
}

void AudioManager::silence() {
    muted_ = true;
    chirpActive_ = false;
    M5.Speaker.stop();
}

void AudioManager::resetMuteIfClear(bool alertActive) {
    if (!alertActive) {
        muted_ = false;
    }
}

void AudioManager::service(uint32_t nowMs) {
    if (!chirpActive_ || muted_) {
        return;
    }
    if (lastToneMs_ != 0 && nowMs - lastToneMs_ < 180) {
        return;
    }

    static constexpr float normalTones[] = {1046.5f, 1318.5f};
    static constexpr float urgentTones[] = {880.0f, 1174.7f, 1568.0f};
    const float* tones = chirpUrgent_ ? urgentTones : normalTones;
    const uint8_t toneCount = chirpUrgent_ ? sizeof(urgentTones) / sizeof(urgentTones[0])
                                           : sizeof(normalTones) / sizeof(normalTones[0]);

    if (toneStep_ >= toneCount) {
        chirpActive_ = false;
        M5.Speaker.stop();
        return;
    }

    const float frequency = tones[toneStep_];
    lastToneMs_ = nowMs;
    toneStep_++;
    M5.Speaker.tone(frequency, chirpUrgent_ ? 120 : 95);
}

}  // namespace aq
