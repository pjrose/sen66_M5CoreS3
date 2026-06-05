#include "ftp_manager.h"

namespace aq {

FtpManager Ftp;

void FtpManager::begin(const AppSettings& settings) {
    setEnabled(settings.ftpEnabled, settings);
}

void FtpManager::setEnabled(bool enabled, const AppSettings& settings) {
    enabled_ = enabled;
    if (enabled_ && !started_) {
        server_.begin(settings.ftpUser.c_str(), settings.ftpPassword.c_str());
        started_ = true;
    }
}

void FtpManager::service() {
    if (enabled_ && started_) {
        server_.handleFTP();
    }
}

}  // namespace aq

