#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <esp_camera.h>
#include "app_types.h"

namespace aq {

class CameraManager {
public:
    bool begin();
    bool capture(uint32_t timestamp, String* savedPath = nullptr);
    bool rotate(size_t maxFiles = 10000);
    size_t recentImages(String* out, size_t capacity);
    bool ready() const { return ready_; }
    String lastError() const { return lastError_; }

private:
    String makePath(uint32_t timestamp) const;

    bool ready_ = false;
    String lastError_;
};

extern CameraManager Camera;

}  // namespace aq
