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
        M5.Speaker.stop();
        toneStep_ = 0;
    }
}

void AudioManager::silence() {
    muted_ = true;
    M5.Speaker.stop();
}

void AudioManager::resetMuteIfClear(bool alertActive) {
    if (!alertActive) {
        muted_ = false;
    }
}

void AudioManager::service(uint32_t nowMs) {
    if (!alarmActive_ || muted_) {
        return;
    }
    if (nowMs - lastToneMs_ < 220) {
        return;
    }
    static constexpr float tones[] = {880.0f, 1174.7f, 1568.0f, 0.0f, 784.0f};
    const float frequency = tones[toneStep_ % (sizeof(tones) / sizeof(tones[0]))];
    lastToneMs_ = nowMs;
    toneStep_++;
    if (frequency <= 0.0f) {
        M5.Speaker.stop();
        return;
    }
    M5.Speaker.tone(frequency, 150);
}

}  // namespace aq

