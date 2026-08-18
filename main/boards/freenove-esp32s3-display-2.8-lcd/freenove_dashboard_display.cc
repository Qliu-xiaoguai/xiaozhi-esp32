#include "freenove_dashboard_display.h"
#include "config.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "application.h"
#include "board.h"
#include "device_state.h"

#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctime>
#include <cstdio>
#include <string>
#include <wifi_manager.h>
#include <noto_emoji.h>
#include <material_symbols.h>

static const char* TAG = "FreenoveDashboard";
static const int kPollIntervalMs = 30000;
static const int kWeatherPollInterval = 10;

static const char* kDayNames[] = {"日", "一", "二", "三", "四", "五", "六"};

static const char* WeatherCodeToChinese(int code) {
    if (code == 0) return "晴";
    if (code <= 3) return "多云";
    if (code <= 49) return "雾";
    if (code <= 59) return "毛毛雨";
    if (code <= 69) return "雨";
    if (code <= 79) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "阵雪";
    if (code <= 99) return "雷暴";
    return "未知";
}

static std::string FormatUptime(int seconds) {
    int days = seconds / 86400;
    int hours = (seconds % 86400) / 3600;
    int mins = (seconds % 3600) / 60;
    char buf[32];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%d天%d时%d分", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, sizeof(buf), "%d时%d分", hours, mins);
    } else {
        snprintf(buf, sizeof(buf), "%d分", mins);
    }
    return buf;
}

static esp_err_t http_data_cb(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto* body = static_cast<std::string*>(evt->user_data);
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

static std::string HttpGet(const char* url, int timeout_ms = 5000) {
    std::string body;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.event_handler = http_data_cb;
    cfg.user_data = &body;

    auto client = esp_http_client_init(&cfg);
    if (!client) return "";

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        esp_http_client_cleanup(client);
        return body;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return "";
}

FreenoveDashboardDisplay::FreenoveDashboardDisplay(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
    data_mutex_ = xSemaphoreCreateMutex();
}

FreenoveDashboardDisplay::~FreenoveDashboardDisplay() {
    running_ = false;
    if (poll_task_) {
        vTaskDelay(pdMS_TO_TICKS(200));
        poll_task_ = nullptr;
    }
    if (data_mutex_) {
        vSemaphoreDelete(data_mutex_);
    }
}

void FreenoveDashboardDisplay::SetupUI() {
    if (setup_ui_called_) return;
    Display::SetupUI();
    DisplayLockGuard lock(this);

    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto* text_font = lvgl_theme->text_font()->font();
    auto* icon_font = lvgl_theme->icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    // Top bar (reuse parent's pattern for network/battery)
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, 4, 0);
    lv_obj_set_style_pad_bottom(top_bar_, 4, 0);
    lv_obj_set_style_pad_left(top_bar_, 8, 0);
    lv_obj_set_style_pad_right(top_bar_, 8, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

    battery_percent_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_percent_label_, "");
    lv_obj_set_style_text_font(battery_percent_label_, text_font, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lvgl_theme->text_color(), 0);

    // Dashboard container
    dashboard_container_ = lv_obj_create(screen);
    lv_obj_set_size(dashboard_container_, LV_HOR_RES, LV_VER_RES - 40);
    lv_obj_align(dashboard_container_, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(dashboard_container_, 0, 0);
    lv_obj_set_style_border_width(dashboard_container_, 0, 0);
    lv_obj_set_style_bg_opa(dashboard_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(dashboard_container_, 8, 0);
    lv_obj_set_style_pad_row(dashboard_container_, 4, 0);
    lv_obj_set_flex_flow(dashboard_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dashboard_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(dashboard_container_, LV_SCROLLBAR_MODE_OFF);

    // Time label (large)
    time_label_ = lv_label_create(dashboard_container_);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_set_style_text_font(time_label_, lvgl_theme->large_icon_font()->font(), 0);
    lv_obj_set_style_text_color(time_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(time_label_, LV_HOR_RES - 16);

    // Date label
    date_label_ = lv_label_create(dashboard_container_);
    lv_label_set_text(date_label_, "");
    lv_obj_set_style_text_font(date_label_, text_font, 0);
    lv_obj_set_style_text_color(date_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(date_label_, LV_HOR_RES - 16);

    // Weather label
    weather_label_ = lv_label_create(dashboard_container_);
    lv_label_set_text(weather_label_, "加载中...");
    lv_obj_set_style_text_font(weather_label_, text_font, 0);
    lv_obj_set_style_text_color(weather_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(weather_label_, LV_HOR_RES - 16);

    // Server stats label
    server_label_ = lv_label_create(dashboard_container_);
    lv_label_set_text(server_label_, "服务器: --");
    lv_obj_set_style_text_font(server_label_, text_font, 0);
    lv_obj_set_style_text_color(server_label_, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(server_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(server_label_, LV_HOR_RES - 16);

    // OTA status label
    ota_label_ = lv_label_create(dashboard_container_);
    lv_label_set_text(ota_label_, "");
    lv_obj_set_style_text_font(ota_label_, text_font, 0);
    lv_obj_set_style_text_color(ota_label_, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(ota_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ota_label_, LV_HOR_RES - 16);

    // Voice assistant container (hidden by default)
    va_container_ = lv_obj_create(screen);
    lv_obj_set_size(va_container_, LV_HOR_RES, LV_VER_RES - 40);
    lv_obj_align(va_container_, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(va_container_, 0, 0);
    lv_obj_set_style_border_width(va_container_, 0, 0);
    lv_obj_set_style_bg_opa(va_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(va_container_, 0, 0);
    lv_obj_set_scrollbar_mode(va_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(va_container_, LV_OBJ_FLAG_HIDDEN);

    va_emoji_label_ = lv_label_create(va_container_);
    lv_obj_set_style_text_font(va_emoji_label_, lvgl_theme->large_icon_font()->font(), 0);
    lv_obj_set_style_text_color(va_emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(va_emoji_label_, "");
    lv_obj_align(va_emoji_label_, LV_ALIGN_CENTER, 0, -30);

    va_status_label_ = lv_label_create(va_container_);
    lv_obj_set_style_text_font(va_status_label_, text_font, 0);
    lv_obj_set_style_text_color(va_status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(va_status_label_, "");
    lv_obj_set_width(va_status_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(va_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(va_status_label_, LV_ALIGN_CENTER, 0, 30);

    va_chat_label_ = lv_label_create(va_container_);
    lv_obj_set_style_text_font(va_chat_label_, text_font, 0);
    lv_obj_set_style_text_color(va_chat_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(va_chat_label_, "");
    lv_obj_set_width(va_chat_label_, LV_HOR_RES - 32);
    lv_obj_set_style_text_align(va_chat_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(va_chat_label_, 4, 0);
    lv_label_set_long_mode(va_chat_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(va_chat_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(va_chat_label_, LV_OBJ_FLAG_HIDDEN);

    // Low battery popup
    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 8, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, "电量低，请充电");
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // Start poll task
    running_ = true;
    xTaskCreatePinnedToCore(PollTaskEntry, "dash_poll", 8192, this, 3, &poll_task_, 0);
}

void FreenoveDashboardDisplay::SwitchToDashboardMode() {
    if (dashboard_mode_) return;
    dashboard_mode_ = true;
    if (dashboard_container_) lv_obj_clear_flag(dashboard_container_, LV_OBJ_FLAG_HIDDEN);
    if (va_container_) lv_obj_add_flag(va_container_, LV_OBJ_FLAG_HIDDEN);
}

void FreenoveDashboardDisplay::SwitchToVoiceMode() {
    if (!dashboard_mode_) return;
    dashboard_mode_ = false;
    if (dashboard_container_) lv_obj_add_flag(dashboard_container_, LV_OBJ_FLAG_HIDDEN);
    if (va_container_) lv_obj_clear_flag(va_container_, LV_OBJ_FLAG_HIDDEN);
}

void FreenoveDashboardDisplay::SetEmotion(const char* emotion) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle || state == kDeviceStateStarting ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating) {
        return;
    }
    DisplayLockGuard lock(this);
    SwitchToVoiceMode();
    if (!va_emoji_label_) return;
    const char* utf8 = noto_emoji_get_utf8(emotion);
    if (!utf8) utf8 = material_symbols_get_utf8(emotion);
    if (utf8) {
        lv_label_set_text(va_emoji_label_, utf8);
    }
}

void FreenoveDashboardDisplay::SetChatMessage(const char* role, const char* content) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle) {
        if (strcmp(role, "system") == 0 && content && content[0] != '\0') {
            if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                ota_text_ = content;
                xSemaphoreGive(data_mutex_);
            }
        }
        return;
    }
    DisplayLockGuard lock(this);
    SwitchToVoiceMode();
    if (!va_chat_label_) return;
    if (content && content[0] != '\0') {
        lv_label_set_text(va_chat_label_, content);
        lv_obj_clear_flag(va_chat_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(va_chat_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void FreenoveDashboardDisplay::SetStatus(const char* status) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle) return;
    DisplayLockGuard lock(this);
    SwitchToVoiceMode();
    if (va_status_label_) {
        lv_label_set_text(va_status_label_, status);
    }
}

void FreenoveDashboardDisplay::ShowNotification(const char* notification, int duration_ms) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle) return;
    DisplayLockGuard lock(this);
    SwitchToVoiceMode();
    if (va_status_label_) {
        lv_label_set_text(va_status_label_, notification);
    }
}

void FreenoveDashboardDisplay::UpdateStatusBar(bool update_all) {
    DisplayLockGuard lock(this);

    // Update mute icon
    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec && mute_label_) {
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_OFF);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update battery
    auto& board = Board::GetInstance();
    int battery_level;
    bool charging, discharging;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", battery_level);
        if (battery_percent_label_) lv_label_set_text(battery_percent_label_, pct);
        if (battery_label_) {
            if (charging) {
                lv_label_set_text(battery_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT);
            } else {
                const char* levels[] = {
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
                };
                int idx = battery_level <= 0 ? 0 : (battery_level >= 100 ? 7 : 1 + ((battery_level - 1) * 6 / 99));
                lv_label_set_text(battery_label_, levels[idx]);
            }
        }
    }

    // Update network every 10 seconds
    static int net_counter = 0;
    if (update_all || net_counter++ % 10 == 0) {
        if (network_label_) {
            const char* icon = board.GetNetworkStateIcon();
            if (icon && network_icon_ != icon) {
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    // Check device state for mode switching
    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle) {
        if (!dashboard_mode_) SwitchToDashboardMode();
    }

    // Update dashboard content
    tick_count_++;
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);

    if (dashboard_mode_ && tm->tm_year >= 2025 - 1900) {
        // Time
        if (time_label_) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);
            lv_label_set_text(time_label_, time_str);
        }
        // Date
        if (date_label_) {
            char date_str[64];
            snprintf(date_str, sizeof(date_str), "%d年%d月%d日 星期%s",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     kDayNames[tm->tm_wday]);
            lv_label_set_text(date_label_, date_str);
        }
        // Weather and server data (from poll task via mutex)
        if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (weather_label_) lv_label_set_text(weather_label_, weather_text_.c_str());
            if (server_label_) lv_label_set_text(server_label_, server_text_.c_str());
            if (ota_label_ && !ota_text_.empty()) lv_label_set_text(ota_label_, ota_text_.c_str());
            xSemaphoreGive(data_mutex_);
        }
    }
}

void FreenoveDashboardDisplay::PollTaskEntry(void* arg) {
    auto* self = static_cast<FreenoveDashboardDisplay*>(arg);
    self->PollTaskLoop();
    vTaskDelete(nullptr);
}

void FreenoveDashboardDisplay::PollTaskLoop() {
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (running_) {
        if (WifiManager::GetInstance().IsConnected()) {
            FetchWeather();
            FetchServerData();
        }
        for (int i = 0; i < kPollIntervalMs / 200 && running_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

void FreenoveDashboardDisplay::FetchWeather() {
    std::string body = HttpGet(WEATHER_API_URL, 8000);
    if (body.empty()) return;

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return;

    cJSON* temp = cJSON_GetObjectItem(root, "temperature");
    cJSON* code = cJSON_GetObjectItem(root, "weather_code");

    if (cJSON_IsNumber(temp) && cJSON_IsNumber(code)) {
        const char* desc = WeatherCodeToChinese(code->valueint);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s %.0f°C", desc, temp->valuedouble);

        if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            weather_text_ = buf;
            xSemaphoreGive(data_mutex_);
        }
    }
    cJSON_Delete(root);
}

void FreenoveDashboardDisplay::FetchServerData() {
    std::string body = HttpGet(SERVER_MONITOR_URL, 5000);
    if (body.empty()) {
        if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            server_text_ = "服务器: 离线";
            xSemaphoreGive(data_mutex_);
        }
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return;

    cJSON* cpu = cJSON_GetObjectItem(root, "cpu");
    cJSON* mem = cJSON_GetObjectItem(root, "memory");
    cJSON* uptime = cJSON_GetObjectItem(root, "uptime");

    char buf[196];
    if (cJSON_IsNumber(cpu) && cJSON_IsObject(mem)) {
        cJSON* mem_pct = cJSON_GetObjectItem(mem, "percent");
        if (cJSON_IsNumber(mem_pct)) {
            std::string uptime_str = "N/A";
            if (cJSON_IsNumber(uptime)) {
                uptime_str = FormatUptime(uptime->valueint);
            }
            snprintf(buf, sizeof(buf), "CPU:%.0f%% 内存:%.0f%% 运行:%s",
                     cpu->valuedouble, mem_pct->valuedouble, uptime_str.c_str());

            if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                server_text_ = buf;
                xSemaphoreGive(data_mutex_);
            }
        }
    } else if (cJSON_IsString(cJSON_GetObjectItem(root, "error"))) {
        if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            server_text_ = "服务器: 错误";
            xSemaphoreGive(data_mutex_);
        }
    }
    cJSON_Delete(root);
}
