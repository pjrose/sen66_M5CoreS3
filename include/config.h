#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include "app_types.h"

namespace aq {

class ConfigManager {
public:
    bool begin();
    bool load();
    bool save() const;
    bool sdReady() const { return sdReady_; }
    const AppSettings& settings() const { return settings_; }
    AppSettings& settings() { return settings_; }
    String lastError() const { return lastError_; }
    bool ensureDataDirectories() const;

private:
    bool mountSd();
    void applyDefaults();

    AppSettings settings_;
    bool sdReady_ = false;
    String lastError_;
};

extern ConfigManager Config;

}  // namespace aq

