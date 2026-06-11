#include "ui_manager.h"

#include <algorithm>
#include <esp_heap_caps.h>
#include <FS.h>
#include <SD.h>

namespace aq {

UiManager Ui;

namespace {
constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kDrawRows = 16;

String fmtFloat(float value, uint8_t decimals, const char* suffix) {
    if (!isfinite(value)) {
        return "--";
    }
    return String(value, static_cast<unsigned int>(decimals)) + suffix;
}

int metricScale(Metric metric) {
    switch (metric) {
        case Metric::Co2: return 1;
        case Metric::Temperature:
        case Metric::Humidity: return 100;
        default: return 10;
    }
}
}

bool UiManager::begin(const AppSettings& settings) {
    Serial.println("[ui] begin");
    lv_init();
    M5.Display.setBrightness(settings.brightness);
    M5.Display.setSwapBytes(true);

    const size_t pixels = kWidth * kDrawRows;
    buf1_ = static_cast<lv_color_t*>(heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!buf1_) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.setTextDatum(top_left);
        M5.Display.drawString("UI init failed", 12, 12);
        M5.Display.drawString("No DMA display buffer", 12, 34);
        Serial.println("[ui] no DMA display buffer");
        return false;
    }
    Serial.printf("[ui] draw buffer ok: %u bytes\r\n", static_cast<unsigned int>(pixels * sizeof(lv_color_t)));

    lv_disp_draw_buf_init(&drawBuf_, buf1_, nullptr, pixels);
    lv_disp_drv_init(&dispDrv_);
    dispDrv_.hor_res = kWidth;
    dispDrv_.ver_res = kHeight;
    dispDrv_.flush_cb = flushCb;
    dispDrv_.draw_buf = &drawBuf_;
    lv_disp_drv_register(&dispDrv_);

    lv_indev_drv_init(&indevDrv_);
    indevDrv_.type = LV_INDEV_TYPE_POINTER;
    indevDrv_.read_cb = touchCb;
    touch_ = lv_indev_drv_register(&indevDrv_);

    createStyles();
    createScreens(settings);
    showScreen(0, false);
    lv_obj_invalidate(screens_[0]);
    lv_refr_now(nullptr);
    Serial.println("[ui] first refresh requested");
    return true;
}

void UiManager::tick() {
    lv_timer_handler();
}

void UiManager::flushCb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* colorP) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    if (Ui.flushLogCount_ < 8) {
        const uint16_t first = reinterpret_cast<uint16_t*>(colorP)[0];
        Serial.printf("[ui] flush %u: x=%d y=%d w=%d h=%d first=0x%04x\r\n",
                      Ui.flushLogCount_,
                      static_cast<int>(area->x1),
                      static_cast<int>(area->y1),
                      static_cast<int>(w),
                      static_cast<int>(h),
                      first);
        Ui.flushLogCount_++;
    }
    M5.Display.startWrite();
    M5.Display.pushImage(area->x1, area->y1, w, h, reinterpret_cast<uint16_t*>(colorP));
    M5.Display.endWrite();
    lv_disp_flush_ready(disp);
}

void UiManager::touchCb(lv_indev_drv_t*, lv_indev_data_t* data) {
    const auto detail = M5.Touch.getDetail();
    if (detail.isPressed()) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = detail.x;
        data->point.y = detail.y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void UiManager::eventCb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT && Ui.currentScreen_ < UiManager::kScreenCount - 1) {
            Ui.showScreen(Ui.currentScreen_ + 1, true);
        } else if (dir == LV_DIR_RIGHT && Ui.currentScreen_ > 0) {
            Ui.showScreen(Ui.currentScreen_ - 1, true);
        }
        return;
    }

    const uintptr_t code = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    switch (code) {
        case 1: Ui.showScreen(0, true); break;
        case 2: Ui.showScreen(1, true); break;
        case 3: Ui.showScreen(2, true); break;
        case 4: Ui.showScreen(3, true); break;
        case 5: Ui.showScreen(4, true); break;
        case 10: Ui.silenceRequested_ = true; break;
        case 20: Ui.monthMode_ = false; Ui.filterChartMode_ = false; Ui.chartModeChanged_ = true; break;
        case 21: Ui.monthMode_ = true; Ui.filterChartMode_ = false; Ui.chartModeChanged_ = true; break;
        case 22: Ui.filterChartMode_ = true; Ui.chartModeChanged_ = true; break;
        case 23:
            if (Ui.eventList_) {
                lv_obj_scroll_to_y(Ui.eventList_, std::max<int32_t>(0, lv_obj_get_scroll_y(Ui.eventList_) - 54), LV_ANIM_ON);
            }
            break;
        case 24:
            if (Ui.eventList_) {
                lv_obj_scroll_to_y(Ui.eventList_, lv_obj_get_scroll_y(Ui.eventList_) + 54, LV_ANIM_ON);
            }
            break;
        case 30: Ui.calibrationRequested_ = true; break;
        case 31: Ui.baselineResetRequested_ = true; break;
        default: break;
    }
    if (code >= 50 && code < 55) {
        const size_t index = code - 50;
        if (Ui.cameraRollPaths_[index].length() > 0) {
            M5.Display.fillScreen(TFT_BLACK);
            File file = SD.open(Ui.cameraRollPaths_[index], FILE_READ);
            if (file) {
                const size_t len = file.size();
                uint8_t* jpg = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!jpg) {
                    jpg = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
                }
                if (jpg && file.read(jpg, len) == static_cast<int>(len)) {
                    M5.Display.drawJpg(jpg, len, 0, 0, 320, 240);
                } else {
                    M5.Display.drawString(Ui.cameraRollPaths_[index], 12, 112);
                }
                if (jpg) {
                    free(jpg);
                }
                file.close();
            }
            delay(2200);
            Ui.showScreen(3, false);
        }
    }
}

void UiManager::createStyles() {
    lv_style_init(&styleBg_);
    lv_style_set_bg_color(&styleBg_, lv_color_hex(0x121212));
    lv_style_set_bg_opa(&styleBg_, LV_OPA_COVER);
    lv_style_set_text_color(&styleBg_, lv_color_hex(0xF5F5F5));

    lv_style_init(&styleGlass_);
    lv_style_set_bg_color(&styleGlass_, lv_color_hex(0x2A2A2A));
    lv_style_set_bg_opa(&styleGlass_, LV_OPA_60);
    lv_style_set_border_color(&styleGlass_, lv_color_hex(0x6E6E6E));
    lv_style_set_border_opa(&styleGlass_, LV_OPA_40);
    lv_style_set_border_width(&styleGlass_, 1);
    lv_style_set_radius(&styleGlass_, 8);
    lv_style_set_pad_all(&styleGlass_, 5);
    lv_style_set_shadow_color(&styleGlass_, lv_color_hex(0x00E676));
    lv_style_set_shadow_opa(&styleGlass_, LV_OPA_20);
    lv_style_set_shadow_width(&styleGlass_, 12);

    lv_style_init(&styleMetric_);
    lv_style_set_text_color(&styleMetric_, lv_color_hex(0xF6F6F6));
    lv_style_set_text_font(&styleMetric_, &lv_font_montserrat_16);

    lv_style_init(&styleAlarm_);
    lv_style_set_bg_color(&styleAlarm_, lv_color_hex(0xFF1744));
    lv_style_set_bg_opa(&styleAlarm_, LV_OPA_90);
    lv_style_set_text_color(&styleAlarm_, lv_color_hex(0xFFFFFF));
    lv_style_set_radius(&styleAlarm_, 4);
}

lv_obj_t* UiManager::createCard(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_add_style(card, &styleGlass_, 0);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void UiManager::createNav(lv_obj_t* screen) {
    const char* labels[] = {"Dash", "Chart", "Log", "Set", "Info"};
    for (uint8_t i = 0; i < kScreenCount; ++i) {
        lv_obj_t* btn = lv_btn_create(screen);
        lv_obj_set_pos(btn, 4 + i * 63, 201);
        lv_obj_set_size(btn, 58, 34);
        lv_obj_set_style_radius(btn, 7, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
        lv_obj_add_event_cb(btn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));

        lv_obj_t* label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_label_set_text(label, labels[i]);
        lv_obj_center(label);

        const uint8_t screenIndex = static_cast<uint8_t>(screen == screens_[0] ? 0 :
                                   screen == screens_[1] ? 1 :
                                   screen == screens_[2] ? 2 :
                                   screen == screens_[3] ? 3 : 4);
        navButtons_[screenIndex][i] = btn;
    }
}

void UiManager::createScreens(const AppSettings& settings) {
    for (auto& screen : screens_) {
        screen = lv_obj_create(nullptr);
        lv_obj_add_style(screen, &styleBg_, 0);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x121212), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(screen, 0, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    }

    statusLabel_ = lv_label_create(screens_[0]);
    lv_obj_set_pos(statusLabel_, 8, 8);
    lv_obj_set_width(statusLabel_, 120);
    lv_obj_set_style_text_font(statusLabel_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(statusLabel_, 1, 0);
    lv_label_set_text(statusLabel_, "WiFi --\nMQTT --\nSD --\n--/-- --:-- --\nACTIVE");

    lv_obj_t* particleCard = createCard(screens_[0], 8, 92, 120, 96);
    primaryCaption_ = lv_label_create(particleCard);
    lv_obj_set_width(primaryCaption_, 108);
    lv_obj_set_style_text_font(primaryCaption_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(primaryCaption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(primaryCaption_, "PM2.5\nug/m3");
    lv_obj_align(primaryCaption_, LV_ALIGN_TOP_MID, 0, 6);

    primaryValue_ = lv_label_create(particleCard);
    lv_obj_set_style_text_font(primaryValue_, &lv_font_montserrat_32, 0);
    lv_label_set_text(primaryValue_, "--");
    lv_obj_align(primaryValue_, LV_ALIGN_BOTTOM_MID, 0, -8);

    const char* names[] = {"Temp", "Hum", "CO2", "VOC", "NOx", "PM1", "PM4", "PM10"};
    for (int i = 0; i < 8; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        lv_obj_t* card = createCard(screens_[0], 136 + col * 88, 8 + row * 46, 84, 40);
        metricLabels_[i] = lv_label_create(card);
        lv_obj_add_style(metricLabels_[i], &styleMetric_, 0);
        lv_obj_set_width(metricLabels_[i], 74);
        lv_obj_set_style_text_font(metricLabels_[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_line_space(metricLabels_[i], 0, 0);
        lv_label_set_text_fmt(metricLabels_[i], "%s\n--", names[i]);
    }

    alarmBanner_ = lv_btn_create(screens_[0]);
    lv_obj_add_style(alarmBanner_, &styleAlarm_, 0);
    lv_obj_set_pos(alarmBanner_, 8, 166);
    lv_obj_set_size(alarmBanner_, 120, 28);
    lv_obj_add_event_cb(alarmBanner_, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(10));
    lv_obj_add_flag(alarmBanner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* alarmText = lv_label_create(alarmBanner_);
    lv_label_set_text(alarmText, "SILENCE ALARM");
    lv_obj_center(alarmText);

    chart_ = lv_chart_create(screens_[1]);
    lv_obj_set_pos(chart_, 8, 8);
    lv_obj_set_size(chart_, 304, 132);
    lv_obj_add_style(chart_, &styleGlass_, 0);
    lv_obj_set_style_bg_color(chart_, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_opa(chart_, LV_OPA_90, 0);
    lv_obj_set_style_border_color(chart_, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_opa(chart_, LV_OPA_60, 0);
    lv_obj_set_style_line_color(chart_, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart_, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart_, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(chart_, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart_, 5, 6);
    lv_chart_set_point_count(chart_, 120);
    lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, 500);
    chartSeries_ = lv_chart_add_series(chart_, lv_color_hex(0x00E676), LV_CHART_AXIS_PRIMARY_Y);
    statsLabel_ = lv_label_create(screens_[1]);
    lv_obj_set_pos(statsLabel_, 10, 146);
    lv_label_set_text(statsLabel_, "Mean --   Min --   Max --");

    lv_obj_t* dayBtn = lv_btn_create(screens_[1]);
    lv_obj_set_pos(dayBtn, 8, 164);
    lv_obj_set_size(dayBtn, 88, 30);
    lv_obj_add_event_cb(dayBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(20));
    lv_obj_t* dayLabel = lv_label_create(dayBtn);
    lv_label_set_text(dayLabel, "24h PM");
    lv_obj_center(dayLabel);

    lv_obj_t* monthBtn = lv_btn_create(screens_[1]);
    lv_obj_set_pos(monthBtn, 104, 164);
    lv_obj_set_size(monthBtn, 88, 30);
    lv_obj_add_event_cb(monthBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(21));
    lv_obj_t* monthLabel = lv_label_create(monthBtn);
    lv_label_set_text(monthLabel, "30d Avg");
    lv_obj_center(monthLabel);

    lv_obj_t* filterBtn = lv_btn_create(screens_[1]);
    lv_obj_set_pos(filterBtn, 200, 164);
    lv_obj_set_size(filterBtn, 88, 30);
    lv_obj_add_event_cb(filterBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(22));
    lv_obj_t* filterLabel = lv_label_create(filterBtn);
    lv_label_set_text(filterLabel, "Filter %");
    lv_obj_center(filterLabel);

    lv_obj_t* debugTitle = lv_label_create(screens_[2]);
    lv_obj_set_pos(debugTitle, 8, 6);
    lv_obj_set_style_text_font(debugTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(debugTitle, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(debugTitle, "Debug");

    lv_obj_t* debugDivider = lv_obj_create(screens_[2]);
    lv_obj_set_pos(debugDivider, 8, 22);
    lv_obj_set_size(debugDivider, 304, 1);
    lv_obj_set_style_bg_color(debugDivider, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_border_width(debugDivider, 0, 0);
    lv_obj_set_style_pad_all(debugDivider, 0, 0);
    lv_obj_clear_flag(debugDivider, LV_OBJ_FLAG_SCROLLABLE);

    debugLabel_ = lv_label_create(screens_[2]);
    lv_obj_set_pos(debugLabel_, 8, 28);
    lv_obj_set_width(debugLabel_, 304);
    lv_obj_set_style_text_font(debugLabel_, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(debugLabel_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(debugLabel_, "Waiting for USB log...");

    lv_obj_t* alarmTitle = lv_label_create(screens_[2]);
    lv_obj_set_pos(alarmTitle, 8, 74);
    lv_obj_set_style_text_font(alarmTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(alarmTitle, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(alarmTitle, "Alarms");

    lv_obj_t* alarmDivider = lv_obj_create(screens_[2]);
    lv_obj_set_pos(alarmDivider, 8, 90);
    lv_obj_set_size(alarmDivider, 304, 1);
    lv_obj_set_style_bg_color(alarmDivider, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_border_width(alarmDivider, 0, 0);
    lv_obj_set_style_pad_all(alarmDivider, 0, 0);
    lv_obj_clear_flag(alarmDivider, LV_OBJ_FLAG_SCROLLABLE);

    eventList_ = lv_list_create(screens_[2]);
    lv_obj_set_pos(eventList_, 8, 98);
    lv_obj_set_size(eventList_, 226, 96);
    lv_obj_add_style(eventList_, &styleGlass_, 0);
    lv_obj_set_style_bg_opa(eventList_, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(eventList_, 2, 0);
    lv_list_add_text(eventList_, "No alarms yet");
    eventPlaceholderShown_ = true;

    lv_obj_t* pageUpBtn = lv_btn_create(screens_[2]);
    lv_obj_set_pos(pageUpBtn, 242, 98);
    lv_obj_set_size(pageUpBtn, 70, 42);
    lv_obj_add_event_cb(pageUpBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(23));
    lv_obj_t* pageUpLabel = lv_label_create(pageUpBtn);
    lv_obj_set_style_text_font(pageUpLabel, &lv_font_montserrat_12, 0);
    lv_label_set_text(pageUpLabel, "PgUp");
    lv_obj_center(pageUpLabel);

    lv_obj_t* pageDownBtn = lv_btn_create(screens_[2]);
    lv_obj_set_pos(pageDownBtn, 242, 152);
    lv_obj_set_size(pageDownBtn, 70, 42);
    lv_obj_add_event_cb(pageDownBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(24));
    lv_obj_t* pageDownLabel = lv_label_create(pageDownBtn);
    lv_obj_set_style_text_font(pageDownLabel, &lv_font_montserrat_12, 0);
    lv_label_set_text(pageDownLabel, "PgDn");
    lv_obj_center(pageDownLabel);

    lv_obj_t* settingsTitle = lv_label_create(screens_[3]);
    lv_obj_set_pos(settingsTitle, 8, 6);
    lv_obj_set_style_text_font(settingsTitle, &lv_font_montserrat_16, 0);
    lv_label_set_text(settingsTitle, "Settings");

    lv_obj_t* brightLabel = lv_label_create(screens_[3]);
    lv_obj_set_pos(brightLabel, 8, 30);
    lv_obj_set_style_text_font(brightLabel, &lv_font_montserrat_12, 0);
    lv_label_set_text(brightLabel, "Brightness");
    brightnessSlider_ = lv_slider_create(screens_[3]);
    lv_obj_set_pos(brightnessSlider_, 108, 33);
    lv_obj_set_size(brightnessSlider_, 190, 12);
    lv_obj_set_style_bg_color(brightnessSlider_, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(brightnessSlider_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(brightnessSlider_, lv_color_hex(0x00E676), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightnessSlider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightnessSlider_, 2, LV_PART_KNOB);
    lv_slider_set_range(brightnessSlider_, 60, 255);
    lv_slider_set_value(brightnessSlider_, std::max<uint8_t>(60, settings.brightness), LV_ANIM_OFF);
    lv_obj_add_event_cb(brightnessSlider_, [](lv_event_t*) { Ui.brightnessChanged_ = true; }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* volumeLabel = lv_label_create(screens_[3]);
    lv_obj_set_pos(volumeLabel, 8, 62);
    lv_obj_set_style_text_font(volumeLabel, &lv_font_montserrat_12, 0);
    lv_label_set_text(volumeLabel, "Volume");
    volumeSlider_ = lv_slider_create(screens_[3]);
    lv_obj_set_pos(volumeSlider_, 108, 65);
    lv_obj_set_size(volumeSlider_, 190, 12);
    lv_obj_set_style_bg_color(volumeSlider_, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(volumeSlider_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(volumeSlider_, lv_color_hex(0x00E676), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volumeSlider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(volumeSlider_, 2, LV_PART_KNOB);
    lv_slider_set_range(volumeSlider_, 0, 255);
    lv_slider_set_value(volumeSlider_, settings.buzzerVolume, LV_ANIM_OFF);
    lv_obj_add_event_cb(volumeSlider_, [](lv_event_t*) { Ui.volumeChanged_ = true; }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* ftpLabel = lv_label_create(screens_[3]);
    lv_obj_set_pos(ftpLabel, 8, 94);
    lv_obj_set_style_text_font(ftpLabel, &lv_font_montserrat_12, 0);
    lv_label_set_text(ftpLabel, "FTP Server");
    ftpSwitch_ = lv_switch_create(screens_[3]);
    lv_obj_set_pos(ftpSwitch_, 108, 88);
    if (settings.ftpEnabled) {
        lv_obj_add_state(ftpSwitch_, LV_STATE_CHECKED);
    }
    ftpEnabled_ = settings.ftpEnabled;
    lv_obj_add_event_cb(ftpSwitch_, [](lv_event_t*) { Ui.ftpChanged_ = true; }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* baselineBtn = lv_btn_create(screens_[3]);
    lv_obj_set_pos(baselineBtn, 8, 122);
    lv_obj_set_size(baselineBtn, 144, 34);
    lv_obj_add_event_cb(baselineBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(31));
    lv_obj_t* baselineBtnLabel = lv_label_create(baselineBtn);
    lv_label_set_text(baselineBtnLabel, "Baseline");
    lv_obj_center(baselineBtnLabel);

    lv_obj_t* calBtn = lv_btn_create(screens_[3]);
    lv_obj_set_pos(calBtn, 168, 122);
    lv_obj_set_size(calBtn, 144, 34);
    lv_obj_add_event_cb(calBtn, eventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(30));
    lv_obj_t* calLabel = lv_label_create(calBtn);
    lv_label_set_text(calLabel, "CO2 Cal");
    lv_obj_center(calLabel);

    baselineStatusLabel_ = lv_label_create(screens_[3]);
    lv_obj_set_pos(baselineStatusLabel_, 8, 162);
    lv_obj_set_width(baselineStatusLabel_, 304);
    lv_obj_set_style_text_font(baselineStatusLabel_, &lv_font_montserrat_12, 0);
    lv_label_set_text(baselineStatusLabel_, "Filter baseline: not set");

    lv_obj_t* maintTitle = lv_label_create(screens_[4]);
    lv_obj_set_pos(maintTitle, 8, 6);
    lv_obj_set_style_text_font(maintTitle, &lv_font_montserrat_16, 0);
    lv_label_set_text(maintTitle, "Maintenance");

    lv_obj_t* maintPanel = lv_obj_create(screens_[4]);
    lv_obj_add_style(maintPanel, &styleGlass_, 0);
    lv_obj_set_pos(maintPanel, 6, 30);
    lv_obj_set_size(maintPanel, 308, 164);
    lv_obj_set_scroll_dir(maintPanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(maintPanel, LV_SCROLLBAR_MODE_AUTO);

    maintenanceLabel_ = lv_label_create(maintPanel);
    lv_obj_set_width(maintenanceLabel_, 286);
    lv_obj_set_style_text_font(maintenanceLabel_, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(maintenanceLabel_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(maintenanceLabel_, "Waiting for diagnostics...");

    for (uint8_t s = 0; s < kScreenCount; ++s) {
        createNav(screens_[s]);
        lv_obj_add_event_cb(screens_[s], eventCb, LV_EVENT_GESTURE, nullptr);
    }
}

void UiManager::showScreen(uint8_t index, bool animate) {
    if (index >= kScreenCount) {
        return;
    }
    currentScreen_ = index;
    for (uint8_t s = 0; s < kScreenCount; ++s) {
        for (uint8_t i = 0; i < kScreenCount; ++i) {
            if (!navButtons_[s][i]) {
                continue;
            }
            const bool active = i == index;
            lv_obj_set_style_bg_color(navButtons_[s][i], lv_color_hex(active ? 0x00E676 : 0x2A2A2A), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(navButtons_[s][i], 0), lv_color_hex(active ? 0x101010 : 0xF5F5F5), 0);
        }
    }
    if (animate) {
        lv_scr_load_anim(screens_[index], LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
    } else {
        lv_scr_load(screens_[index]);
    }
}

void UiManager::updateSample(const SensorSample& sample) {
    if (primaryArc_) {
        lv_arc_set_value(primaryArc_, isfinite(sample.pm2p5) ? std::min<int>(500, sample.pm2p5 * 10) : 0);
        lv_obj_set_style_arc_color(primaryArc_, aqiColor(sample), LV_PART_INDICATOR);
    }
    lv_obj_set_style_text_color(primaryValue_, aqiColor(sample), 0);
    lv_label_set_text(primaryValue_, isfinite(sample.pm2p5) ? String(sample.pm2p5, 1).c_str() : "--");
    lv_label_set_text(metricLabels_[0], ("Temp\n" + fmtFloat(sample.temperature, 1, " C")).c_str());
    lv_label_set_text(metricLabels_[1], ("Hum\n" + fmtFloat(sample.humidity, 0, "%")).c_str());
    lv_label_set_text_fmt(metricLabels_[2], "CO2\n%u ppm", sample.co2);
    lv_label_set_text(metricLabels_[3], ("VOC\n" + fmtFloat(sample.vocIndex, 0, "")).c_str());
    lv_label_set_text(metricLabels_[4], ("NOx\n" + fmtFloat(sample.noxIndex, 0, "")).c_str());
    lv_label_set_text(metricLabels_[5], ("PM1\n" + fmtFloat(sample.pm1p0, 1, "")).c_str());
    lv_label_set_text(metricLabels_[6], ("PM4\n" + fmtFloat(sample.pm4p0, 1, "")).c_str());
    lv_label_set_text(metricLabels_[7], ("PM10\n" + fmtFloat(sample.pm10p0, 1, "")).c_str());
}

void UiManager::updateStatus(bool wifi, bool mqtt, bool sd, DeviceState state) {
    const char* stateText = "ACTIVE";
    if (state == DeviceState::Warmup) stateText = "WARM";
    else if (state == DeviceState::Measuring) stateText = "MEAS";
    else if (state == DeviceState::LightSleep) stateText = "SLEEP";
    else if (state == DeviceState::Error) stateText = "ERROR";

    time_t now = time(nullptr);
    struct tm tmv {};
    localtime_r(&now, &tmv);
    char timeText[18] = "--/-- --:-- --";
    if (now > 1700000000) {
        strftime(timeText, sizeof(timeText), "%m/%d %I:%M %p", &tmv);
    }
    lv_label_set_text_fmt(statusLabel_, "WiFi %s\nMQTT %s\nSD %s\n%s\n%s",
                          wifi ? "ok" : "--", mqtt ? "ok" : "--", sd ? "ok" : "--",
                          timeText, stateText);
}

void UiManager::updateAlarm(bool active, bool muted) {
    if (active) {
        lv_obj_clear_flag(alarmBanner_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(alarmBanner_, muted ? LV_OPA_50 : LV_OPA_90, 0);
    } else {
        lv_obj_add_flag(alarmBanner_, LV_OBJ_FLAG_HIDDEN);
    }
}

void UiManager::setStats(const char* meanText, const char* minText, const char* maxText) {
    lv_label_set_text_fmt(statsLabel_, "Mean %s   Min %s   Max %s", meanText, minText, maxText);
}

void UiManager::addEvent(const String& eventText) {
    if (eventPlaceholderShown_) {
        lv_obj_clean(eventList_);
        eventPlaceholderShown_ = false;
    }
    lv_list_add_text(eventList_, eventText.c_str());
    lv_obj_scroll_to_y(eventList_, LV_COORD_MAX, LV_ANIM_ON);
}

void UiManager::updateDebugMessage(const String& text) {
    if (!debugLabel_) {
        return;
    }
    lv_label_set_text(debugLabel_, text.c_str());
}

void UiManager::plotHistory(const HistoryPoint* points, size_t count, Metric metric) {
    if (!points || count == 0) {
        return;
    }
    lv_chart_set_point_count(chart_, std::min<size_t>(120, count));
    int16_t minValue = INT16_MAX;
    int16_t maxValue = INT16_MIN;
    int32_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int16_t value = points[i].value == INT16_MAX ? 0 : points[i].value;
        lv_chart_set_next_value(chart_, chartSeries_, value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        sum += value;
    }
    lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, std::min<int16_t>(0, minValue), std::max<int16_t>(100, maxValue + 10));
    lv_chart_refresh(chart_);

    const int scale = metricScale(metric);
    char mean[16], minT[16], maxT[16];
    snprintf(mean, sizeof(mean), "%.1f", (sum / static_cast<float>(count)) / scale);
    snprintf(minT, sizeof(minT), "%.1f", minValue / static_cast<float>(scale));
    snprintf(maxT, sizeof(maxT), "%.1f", maxValue / static_cast<float>(scale));
    setStats(mean, minT, maxT);
}

void UiManager::plotFilterDeviation(const HistoryPoint* points, size_t count, const FilterDeviationSummary& summary) {
    if (!points || count == 0) {
        lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        setStats("wait", "wait", "wait");
        return;
    }
    lv_chart_set_point_count(chart_, std::min<size_t>(120, count));
    int16_t minValue = INT16_MAX;
    int16_t maxValue = INT16_MIN;
    for (size_t i = 0; i < count; ++i) {
        const int16_t value = points[i].value;
        lv_chart_set_next_value(chart_, chartSeries_, value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }
    lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, std::min<int16_t>(-20, minValue), std::max<int16_t>(100, maxValue + 10));
    lv_chart_refresh(chart_);

    char current[16], peak[16], mean[16];
    snprintf(current, sizeof(current), "%d%%", summary.currentPercent);
    snprintf(peak, sizeof(peak), "%d%%", summary.peakPercent);
    snprintf(mean, sizeof(mean), "%d%%", summary.meanPercent);
    lv_label_set_text_fmt(statsLabel_, "Now %s   Peak %s   Avg %s", current, peak, mean);
}

void UiManager::updateCameraRoll(const String* paths, size_t count) {
    for (size_t i = 0; i < 5; ++i) {
        if (!cameraRollLabels_[i]) {
            continue;
        }
        if (i < count) {
            cameraRollPaths_[i] = paths[i];
            const int slash = paths[i].lastIndexOf('/');
            const String name = slash >= 0 ? paths[i].substring(slash + 1) : paths[i];
            lv_label_set_text(cameraRollLabels_[i], name.substring(4, 10).c_str());
        } else {
            cameraRollPaths_[i].clear();
            lv_label_set_text(cameraRollLabels_[i], "--");
        }
    }
}

void UiManager::updateBaselineStatus(const AppSettings& settings) {
    if (!baselineStatusLabel_) {
        return;
    }
    if (settings.filterBaselineActive) {
        const uint32_t now = static_cast<uint32_t>(time(nullptr));
        const uint32_t elapsed = now > settings.filterBaselineStarted ? now - settings.filterBaselineStarted : 0;
        const uint16_t doneHours = static_cast<uint16_t>(std::min<uint32_t>(settings.filterBaselineHours, elapsed / 3600UL));
        lv_label_set_text_fmt(baselineStatusLabel_, "Filter baseline: learning %uh/%uh", doneHours, settings.filterBaselineHours);
        return;
    }
    if (settings.filterBaselineReady) {
        lv_label_set_text_fmt(baselineStatusLabel_, "Filter baseline: PM2.5 %.1f  PM10 %.1f", settings.filterBaselinePm25, settings.filterBaselinePm10);
        return;
    }
    lv_label_set_text(baselineStatusLabel_, "Filter baseline: not set");
}

void UiManager::updateMaintenance(const String& text) {
    if (maintenanceLabel_) {
        lv_label_set_text(maintenanceLabel_, text.c_str());
    }
}

bool UiManager::silenceRequested() {
    const bool value = silenceRequested_;
    silenceRequested_ = false;
    return value;
}

bool UiManager::brightnessChanged(uint8_t& value) {
    if (!brightnessChanged_) {
        return false;
    }
    brightnessChanged_ = false;
    value = static_cast<uint8_t>(std::max<int>(60, lv_slider_get_value(brightnessSlider_)));
    return true;
}

bool UiManager::volumeChanged(uint8_t& value) {
    if (!volumeChanged_) {
        return false;
    }
    volumeChanged_ = false;
    value = static_cast<uint8_t>(lv_slider_get_value(volumeSlider_));
    return true;
}

bool UiManager::ftpToggleChanged(bool& enabled) {
    if (!ftpChanged_) {
        return false;
    }
    ftpChanged_ = false;
    ftpEnabled_ = lv_obj_has_state(ftpSwitch_, LV_STATE_CHECKED);
    enabled = ftpEnabled_;
    return true;
}

bool UiManager::calibrationRequested() {
    const bool value = calibrationRequested_;
    calibrationRequested_ = false;
    return value;
}

bool UiManager::baselineResetRequested() {
    const bool value = baselineResetRequested_;
    baselineResetRequested_ = false;
    return value;
}

void UiManager::goToScreen(uint8_t index, bool animate) {
    showScreen(index, animate);
}

bool UiManager::handleRawTouch(int16_t x, int16_t y) {
    if (x < 0 || y < 0) {
        return false;
    }

    if (y >= 198) {
        const uint8_t target = static_cast<uint8_t>(std::min(4, std::max(0, x / 64)));
        showScreen(target, true);
        return true;
    }

    if (currentScreen_ == 1 && y >= 158 && y <= 196) {
        if (x < 100) {
            monthMode_ = false;
            filterChartMode_ = false;
        } else if (x < 196) {
            monthMode_ = true;
            filterChartMode_ = false;
        } else {
            filterChartMode_ = true;
        }
        chartModeChanged_ = true;
        return true;
    }

    if (currentScreen_ == 2 && x >= 238) {
        if (y >= 94 && y < 146) {
            lv_obj_scroll_to_y(eventList_, std::max<int32_t>(0, lv_obj_get_scroll_y(eventList_) - 54), LV_ANIM_ON);
            return true;
        }
        if (y >= 146 && y < 198) {
            lv_obj_scroll_to_y(eventList_, lv_obj_get_scroll_y(eventList_) + 54, LV_ANIM_ON);
            return true;
        }
    }

    if (currentScreen_ == 3) {
        if (y >= 24 && y < 52) {
            const int mapped = map(constrain(x, 108, 298), 108, 298, 60, 255);
            lv_slider_set_value(brightnessSlider_, mapped, LV_ANIM_OFF);
            brightnessChanged_ = true;
            return true;
        }
        if (y >= 56 && y < 84) {
            const int mapped = map(constrain(x, 108, 298), 108, 298, 0, 255);
            lv_slider_set_value(volumeSlider_, mapped, LV_ANIM_OFF);
            volumeChanged_ = true;
            return true;
        }
        if (y >= 86 && y < 120) {
            ftpEnabled_ = !ftpEnabled_;
            if (ftpEnabled_) {
                lv_obj_add_state(ftpSwitch_, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(ftpSwitch_, LV_STATE_CHECKED);
            }
            ftpChanged_ = true;
            return true;
        }
        if (y >= 118 && y < 160) {
            if (x < 160) {
                baselineResetRequested_ = true;
            } else {
                calibrationRequested_ = true;
            }
            return true;
        }
    }

    return false;
}

bool UiManager::chartModeChanged() {
    const bool value = chartModeChanged_;
    chartModeChanged_ = false;
    return value;
}

lv_color_t UiManager::aqiColor(const SensorSample& sample) const {
    if (sample.co2 >= 1500 || sample.pm2p5 >= 55.0f || sample.vocIndex >= 300) {
        return lv_color_hex(0xFF1744);
    }
    if (sample.co2 >= 1000 || sample.pm2p5 >= 25.0f || sample.vocIndex >= 150) {
        return lv_color_hex(0xFFD600);
    }
    return lv_color_hex(0x00E676);
}

void UiManager::setLabel(lv_obj_t* label, const String& text) {
    lv_label_set_text(label, text.c_str());
}

}  // namespace aq
