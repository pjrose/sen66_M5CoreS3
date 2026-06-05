#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <M5Unified.h>
#include "app_types.h"

namespace aq {

class UiManager {
public:
    bool begin(const AppSettings& settings);
    void tick();
    void updateSample(const SensorSample& sample);
    void updateStatus(bool wifi, bool mqtt, bool sd, DeviceState state);
    void updateAlarm(bool active, bool muted);
    void setStats(const char* meanText, const char* minText, const char* maxText);
    void addEvent(const String& eventText);
    void plotHistory(const HistoryPoint* points, size_t count, Metric metric);
    void updateCameraRoll(const String* paths, size_t count);
    bool silenceRequested();
    bool brightnessChanged(uint8_t& value);
    bool volumeChanged(uint8_t& value);
    bool ftpToggleChanged(bool& enabled);
    bool calibrationRequested();
    Metric selectedMetric() const { return selectedMetric_; }
    bool monthMode() const { return monthMode_; }

private:
    static void flushCb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* colorP);
    static void touchCb(lv_indev_drv_t* indev, lv_indev_data_t* data);
    static void eventCb(lv_event_t* e);

    void createStyles();
    void createScreens(const AppSettings& settings);
    lv_obj_t* createCard(lv_obj_t* parent, int x, int y, int w, int h);
    void setLabel(lv_obj_t* label, const String& text);
    lv_color_t aqiColor(const SensorSample& sample) const;
    void showScreen(uint8_t index, bool animate);

    lv_disp_draw_buf_t drawBuf_;
    lv_color_t* buf1_ = nullptr;
    lv_color_t* buf2_ = nullptr;
    lv_disp_drv_t dispDrv_;
    lv_indev_drv_t indevDrv_;
    lv_indev_t* touch_ = nullptr;

    lv_style_t styleBg_;
    lv_style_t styleGlass_;
    lv_style_t styleMetric_;
    lv_style_t styleAlarm_;

    lv_obj_t* screens_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* statusLabel_ = nullptr;
    lv_obj_t* primaryArc_ = nullptr;
    lv_obj_t* primaryValue_ = nullptr;
    lv_obj_t* primaryCaption_ = nullptr;
    lv_obj_t* metricLabels_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* alarmBanner_ = nullptr;
    lv_obj_t* chart_ = nullptr;
    lv_chart_series_t* chartSeries_ = nullptr;
    lv_obj_t* statsLabel_ = nullptr;
    lv_obj_t* eventList_ = nullptr;
    lv_obj_t* brightnessSlider_ = nullptr;
    lv_obj_t* volumeSlider_ = nullptr;
    lv_obj_t* ftpSwitch_ = nullptr;
    lv_obj_t* cameraRollLabels_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    String cameraRollPaths_[5];

    bool silenceRequested_ = false;
    bool brightnessChanged_ = false;
    bool volumeChanged_ = false;
    bool ftpChanged_ = false;
    bool calibrationRequested_ = false;
    bool ftpEnabled_ = false;
    uint8_t currentScreen_ = 0;
    Metric selectedMetric_ = Metric::Pm25;
    bool monthMode_ = false;
};

extern UiManager Ui;

}  // namespace aq
