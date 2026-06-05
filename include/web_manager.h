#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "app_types.h"

namespace aq {

class WebManager {
public:
    bool begin();
    void service();
    void updateSample(const SensorSample& sample, bool alertActive, DeviceState state);
    void updateNetwork(bool wifiConnected, bool mqttConnected);

private:
    void registerRoutes();
    void sendDashboard();
    void sendLiveJson();
    void sendHistoryJson();
    void sendFileList();
    void sendDownload();
    void sendImage();
    void sendNotFound();

    Metric metricFromArg(const String& value) const;
    const char* stateName(DeviceState state) const;
    bool allowedPath(const String& path) const;
    String contentType(const String& path) const;
    float scaledValue(Metric metric, int16_t value) const;

    WebServer server_{80};
    SemaphoreHandle_t mutex_ = nullptr;
    SensorSample latest_;
    bool sampleReady_ = false;
    bool alertActive_ = false;
    bool wifiConnected_ = false;
    bool mqttConnected_ = false;
    DeviceState state_ = DeviceState::Active;
};

extern WebManager Web;

}  // namespace aq

