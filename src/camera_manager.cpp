#include "camera_manager.h"

#include <algorithm>
#include <img_converters.h>
#include <M5Unified.h>
#include <time.h>
#include <vector>

namespace aq {

CameraManager Camera;

namespace {
camera_config_t makeCoreS3CameraConfig() {
    camera_config_t config {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = -1;
    config.pin_sccb_sda = 12;
    config.pin_sccb_scl = 11;
    config.pin_d7 = 47;
    config.pin_d6 = 48;
    config.pin_d5 = 16;
    config.pin_d4 = 15;
    config.pin_d3 = 42;
    config.pin_d2 = 41;
    config.pin_d1 = 40;
    config.pin_d0 = 39;
    config.pin_vsync = 46;
    config.pin_href = 38;
    config.pin_pclk = 45;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = psramFound() ? 2 : 1;
    config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.sccb_i2c_port = -1;
    return config;
}

String two(int value) {
    return value < 10 ? "0" + String(value) : String(value);
}
}

bool CameraManager::begin() {
    M5.In_I2C.release();
    camera_config_t config = makeCoreS3CameraConfig();
    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        lastError_ = String("Camera init failed: ") + String(err);
        ready_ = false;
        return false;
    }
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    }
    ready_ = true;
    return true;
}

bool CameraManager::capture(uint32_t timestamp, String* savedPath) {
    if (!ready_) {
        if (!begin()) {
            return false;
        }
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        lastError_ = "Camera frame unavailable";
        return false;
    }

    uint8_t* jpg = nullptr;
    size_t jpgLen = 0;
    bool converted = false;
    if (fb->format == PIXFORMAT_JPEG) {
        jpg = fb->buf;
        jpgLen = fb->len;
        converted = true;
    } else {
        converted = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 84, &jpg, &jpgLen);
    }

    if (!converted || !jpg || jpgLen == 0) {
        esp_camera_fb_return(fb);
        lastError_ = "JPEG conversion failed";
        return false;
    }

    if (!SD.exists("/cam")) {
        SD.mkdir("/cam");
    }
    const String path = makePath(timestamp);
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        if (fb->format != PIXFORMAT_JPEG && jpg) {
            free(jpg);
        }
        esp_camera_fb_return(fb);
        lastError_ = "Camera file open failed";
        return false;
    }
    const size_t written = file.write(jpg, jpgLen);
    file.close();

    if (fb->format != PIXFORMAT_JPEG && jpg) {
        free(jpg);
    }
    esp_camera_fb_return(fb);

    if (written != jpgLen) {
        lastError_ = "Camera file write short";
        return false;
    }
    if (savedPath) {
        *savedPath = path;
    }
    rotate(10000);
    return true;
}

bool CameraManager::rotate(size_t maxFiles) {
    File dir = SD.open("/cam");
    if (!dir || !dir.isDirectory()) {
        return false;
    }
    std::vector<String> names;
    for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
        const String name = file.name();
        if (!file.isDirectory() && name.endsWith(".jpg")) {
            names.push_back(name.startsWith("/") ? name : "/cam/" + name);
        }
        file.close();
    }
    dir.close();
    std::sort(names.begin(), names.end());
    while (names.size() > maxFiles) {
        SD.remove(names.front());
        names.erase(names.begin());
    }
    return true;
}

size_t CameraManager::recentImages(String* out, size_t capacity) {
    if (!out || capacity == 0) {
        return 0;
    }
    File dir = SD.open("/cam");
    if (!dir || !dir.isDirectory()) {
        return 0;
    }
    std::vector<String> names;
    for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
        const String name = file.name();
        if (!file.isDirectory() && name.endsWith(".jpg")) {
            names.push_back(name.startsWith("/") ? name : "/cam/" + name);
        }
        file.close();
    }
    dir.close();
    std::sort(names.begin(), names.end(), [](const String& a, const String& b) { return a > b; });
    const size_t count = std::min(capacity, names.size());
    for (size_t i = 0; i < count; ++i) {
        out[i] = names[i];
    }
    return count;
}

String CameraManager::makePath(uint32_t timestamp) const {
    time_t t = timestamp;
    struct tm tmv {};
    localtime_r(&t, &tmv);
    return "/cam/pic_" + String(tmv.tm_year + 1900) + two(tmv.tm_mon + 1) + two(tmv.tm_mday) + "_" +
           two(tmv.tm_hour) + two(tmv.tm_min) + two(tmv.tm_sec) + ".jpg";
}

}  // namespace aq
