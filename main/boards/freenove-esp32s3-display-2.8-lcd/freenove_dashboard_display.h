#pragma once

#include "display/lcd_display.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string>

class FreenoveDashboardDisplay : public SpiLcdDisplay {
public:
    FreenoveDashboardDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy);
    ~FreenoveDashboardDisplay() override;

    void SetupUI() override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;

private:
    static void PollTaskEntry(void* arg);
    void PollTaskLoop();
    void FetchWeather();
    void FetchServerData();
    void UpdateDashboardUI();
    void SwitchToDashboardMode();
    void SwitchToVoiceMode();

    TaskHandle_t poll_task_ = nullptr;
    volatile bool running_ = false;
    bool dashboard_mode_ = true;

    lv_obj_t* dashboard_container_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* server_label_ = nullptr;
    lv_obj_t* ota_label_ = nullptr;

    lv_obj_t* va_container_ = nullptr;
    lv_obj_t* va_emoji_label_ = nullptr;
    lv_obj_t* va_status_label_ = nullptr;
    lv_obj_t* va_chat_label_ = nullptr;

    SemaphoreHandle_t data_mutex_ = nullptr;
    std::string weather_text_ = "--";
    std::string server_text_ = "--";
    std::string ota_text_;
    int tick_count_ = 0;
};
