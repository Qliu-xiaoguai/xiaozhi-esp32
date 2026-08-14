#include "freenove_lcd_display.h"
#include "config.h"
#include "display/lvgl_theme.h"

#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>

#define TAG "FreenoveLcd"

static const int kPollIntervalMs = 10000;

FreenoveLcdDisplay::FreenoveLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_handle_t panel,
                                       int width, int height,
                                       int offset_x, int offset_y,
                                       bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
}

FreenoveLcdDisplay::~FreenoveLcdDisplay() {
    running_ = false;
    if (poll_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
        poll_task_ = nullptr;
    }
}

void FreenoveLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto* text_font = lvgl_theme->text_font()->font();

    balance_label_ = lv_label_create(lv_screen_active());
    lv_label_set_text(balance_label_, "--");
    lv_obj_set_style_text_font(balance_label_, text_font, 0);
    lv_obj_set_style_text_color(balance_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(balance_label_, LV_ALIGN_BOTTOM_RIGHT, -8, -8);

    running_ = true;
    xTaskCreate(PollTaskEntry, "balance_poll", 4096, this, 3, &poll_task_);
}

void FreenoveLcdDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);

    DisplayLockGuard lock(this);
    if (balance_label_ != nullptr) {
        auto* lvgl_theme = static_cast<LvglTheme*>(theme);
        lv_obj_set_style_text_color(balance_label_, lvgl_theme->text_color(), 0);
    }
}

void FreenoveLcdDisplay::PollTaskEntry(void* arg) {
    auto* self = static_cast<FreenoveLcdDisplay*>(arg);
    self->PollTaskLoop();
    vTaskDelete(nullptr);
}

void FreenoveLcdDisplay::PollTaskLoop() {
    while (running_) {
        FetchBalance();
        for (int i = 0; i < kPollIntervalMs / 200 && running_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static esp_err_t http_data_cb(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto* body = static_cast<std::string*>(evt->user_data);
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

void FreenoveLcdDisplay::FetchBalance() {
    std::string body;

    esp_http_client_config_t cfg = {};
    cfg.url = BALANCE_API_URL;
    cfg.timeout_ms = 5000;
    cfg.event_handler = http_data_cb;
    cfg.user_data = &body;

    auto client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGW(TAG, "http init failed");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200 && !body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* bal = cJSON_GetObjectItem(root, "balance");
            if (cJSON_IsNumber(bal)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "\xC2\xA5%.2f", bal->valuedouble);
                balance_text_ = buf;
                UpdateBalanceLabel();
            }
            cJSON_Delete(root);
        }
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

void FreenoveLcdDisplay::UpdateBalanceLabel() {
    if (balance_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(balance_label_, balance_text_.c_str());
}
