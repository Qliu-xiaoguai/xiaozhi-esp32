#pragma once

#include "display/lcd_display.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>

class FreenoveLcdDisplay : public SpiLcdDisplay {
public:
    FreenoveLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height, int offset_x, int offset_y,
                       bool mirror_x, bool mirror_y, bool swap_xy);
    ~FreenoveLcdDisplay() override;

    void SetupUI() override;
    void SetTheme(Theme* theme) override;

private:
    lv_obj_t* balance_label_ = nullptr;
    TaskHandle_t poll_task_ = nullptr;
    std::string balance_text_ = "--";
    volatile bool running_ = false;

    static void PollTaskEntry(void* arg);
    void PollTaskLoop();
    void FetchBalance();
    void UpdateBalanceLabel();
};
