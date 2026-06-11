#include "ftp_manager.h"

#include <WiFi.h>

namespace aq {

FtpManager Ftp;

void FtpManager::begin(const AppSettings& settings) {
    user_ = settings.ftpUser;
    password_ = settings.ftpPassword;
    enabled_ = settings.ftpEnabled;
}

void FtpManager::setEnabled(bool enabled, const AppSettings& settings) {
    user_ = settings.ftpUser;
    password_ = settings.ftpPassword;
    enabled_ = enabled;
}

void FtpManager::service() {
    if (enabled_ && !started_) {
        if (WiFi.status() != WL_CONNECTED) {
            return;
        }
        server_.begin(user_.c_str(), password_.c_str());
        started_ = true;
    }
    if (enabled_ && started_) {
        server_.handleFTP();
    }
}

}  // namespace aq
