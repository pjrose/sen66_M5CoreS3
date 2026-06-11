#pragma once

#include <Arduino.h>
#include <SimpleFTPServer.h>
#include "app_types.h"

namespace aq {

class FtpManager {
public:
    void begin(const AppSettings& settings);
    void setEnabled(bool enabled, const AppSettings& settings);
    void service();
    bool enabled() const { return enabled_; }

private:
    FtpServer server_;
    String user_;
    String password_;
    bool enabled_ = false;
    bool started_ = false;
};

extern FtpManager Ftp;

}  // namespace aq
