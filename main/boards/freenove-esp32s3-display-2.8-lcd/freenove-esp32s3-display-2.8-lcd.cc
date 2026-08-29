#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "adc_battery_monitor.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include "led/single_led.h"
#include "system_reset.h"
#include "esp_lcd_ili9341.h"


#include <cJSON.h>
#include "display/lvgl_display/lvgl_theme.h"

#define LAUNCHER_TAG "Launcher"

static void MapTouch(uint16_t tx, uint16_t ty, lv_coord_t& x, lv_coord_t& y) {
    x = (lv_coord_t)ty;
    y = (lv_coord_t)tx;
}

#define GRID_COLS 3
#define GRID_ROWS 2
#define ICON_W (320 / GRID_COLS)
#define ICON_H (240 / GRID_ROWS)

#define TAG "FreenoveESP32S3Display"

class TouchDriver {
public:
    TouchDriver() : dev_(nullptr) {}

    bool Init(i2c_master_bus_handle_t bus, uint8_t addr) {
        i2c_device_config_t cfg = {
            .device_address = addr,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
        };
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool Read(bool &touched, uint16_t &x, uint16_t &y) {
        touched = false;
        x = y = 0;
        if (!dev_) return false;

        uint8_t reg = 0x02;
        uint8_t buf[5];
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK) return false;

        uint8_t points = buf[0] & 0x0F;
        if (points == 0) return true;

        touched = true;
        x = ((buf[1] & 0x0F) << 8) | buf[2];
        y = ((buf[3] & 0x0F) << 8) | buf[4];
        return true;
    }

private:
    i2c_master_dev_handle_t dev_;
};

class FreenoveESP32S3Display : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay *display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    TouchDriver touch_;
    AdcBatteryMonitor* adc_battery_monitor_;

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    static void TouchTask(void *arg) {
        auto *self = static_cast<FreenoveESP32S3Display*>(arg);
        auto &app = Application::GetInstance();

        uint32_t last_tap = 0;
        uint32_t down_start = 0;
        bool down = false;
        uint16_t down_x = 0, down_y = 0;

        while (true) {
            if (!self->ui_ready_) {
                auto d = self->display_;
                if (d && d->IsSetupUICalled()) {
                    DisplayLockGuard lock(d);
                    self->SetupLauncherUI();
                    self->ui_ready_ = true;
                    ESP_LOGI(LAUNCHER_TAG, "Launcher UI ready");
                }
            }

            bool t;
            uint16_t x, y;
            self->touch_.Read(t, x, y);
            uint32_t now = esp_timer_get_time() / 1000;

            if (t) {
                if (!down) {
                    down = true;
                    down_start = now;
                    down_x = x; down_y = y;
                }
            }

            if (!t && down) {
                down = false;
                uint32_t press = now - down_start;
                if (press > 3000) {
                    self->EnterWifiConfigMode();
                    continue;
                }
                lv_coord_t sx, sy;
                MapTouch(down_x, down_y, sx, sy);
                if (self->ui_mode_ == UiMode::Home) {
                    self->HandleHomeTap(sx, sy);
                } else if (self->ui_mode_ == UiMode::Monitor) {
                    self->HandleMonitorTap(sx, sy);
                } else {
                    if (press < 250 && now - last_tap < 250) {
                        app.StartListening();
                        last_tap = 0;
                    } else {
                        app.ToggleChatState();
                        last_tap = now;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeTouch() {
        if (!touch_.Init(codec_i2c_bus_, 0x38)) return;
        xTaskCreatePinnedToCore(TouchTask, "touch_task", 4096, this, 5, nullptr, 0);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MIS0_PIN;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "Install LCD driver ILI9341");
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTools() {
    }

public:
    FreenoveESP32S3Display(): boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->SetBrightness(100);
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, AUDIO_CODEC_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override { return display_; }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};


    // ===== 车机主页 + 服务器监控页（小番定制 v2.5.1） =====
    enum class UiMode { Home, Chat, Monitor };
    UiMode ui_mode_ = UiMode::Home;
    bool ui_ready_ = false;

    lv_obj_t* home_layer_ = nullptr;
    lv_obj_t* monitor_layer_ = nullptr;
    lv_obj_t* mon_bars_[3] = {};
    lv_obj_t* mon_labels_[3] = {};
    lv_obj_t* mon_svc_label_ = nullptr;
    lv_obj_t* mon_time_label_ = nullptr;

    void SetupLauncherUI() {
        auto theme = static_cast<LvglTheme*>(display_->GetTheme());
        if (theme == nullptr) return;
        auto text_font = theme->text_font()->font();
        auto screen = lv_screen_active();

        home_layer_ = lv_obj_create(screen);
        lv_obj_set_size(home_layer_, 320, 240);
        lv_obj_set_pos(home_layer_, 0, 0);
        lv_obj_set_style_bg_color(home_layer_, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(home_layer_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(home_layer_, 0, 0);
        lv_obj_set_style_border_width(home_layer_, 0, 0);
        lv_obj_set_style_pad_all(home_layer_, 0, 0);

        lv_obj_t* title = lv_label_create(home_layer_);
        lv_label_set_text(title, "小番车机");
        lv_obj_set_style_text_font(title, text_font, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        const char* names[6] = {"语音助手", "服务器监控", "天气", "音乐", "新闻", "设置"};
        const uint32_t colors[6] = {0x2E7D32, 0x1565C0, 0xF9A825, 0x6A1B9A, 0xE65100, 0x455A64};
        for (int i = 0; i < 6; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;
            lv_obj_t* card = lv_obj_create(home_layer_);
            lv_obj_set_size(card, ICON_W - 16, ICON_H - 20);
            lv_obj_set_pos(card, col * ICON_W + 8, row * ICON_H + 26);
            lv_obj_set_style_bg_color(card, lv_color_hex(colors[i]), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(card, 12, 0);
            lv_obj_set_style_border_width(card, 0, 0);
            lv_obj_set_style_shadow_width(card, 0, 0);
            lv_obj_t* label = lv_label_create(card);
            lv_label_set_text(label, names[i]);
            lv_obj_set_style_text_font(label, text_font, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(label);
        }

        monitor_layer_ = lv_obj_create(screen);
        lv_obj_set_size(monitor_layer_, 320, 240);
        lv_obj_set_pos(monitor_layer_, 0, 0);
        lv_obj_set_style_bg_color(monitor_layer_, lv_color_hex(0x0D1117), 0);
        lv_obj_set_style_bg_opa(monitor_layer_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(monitor_layer_, 0, 0);
        lv_obj_set_style_border_width(monitor_layer_, 0, 0);
        lv_obj_set_style_pad_all(monitor_layer_, 0, 0);

        lv_obj_t* back = lv_obj_create(monitor_layer_);
        lv_obj_set_size(back, 60, 36);
        lv_obj_set_pos(back, 8, 8);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x30363D), 0);
        lv_obj_set_style_radius(back, 8, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_t* back_label = lv_label_create(back);
        lv_label_set_text(back_label, "<- 返回");
        lv_obj_set_style_text_font(back_label, text_font, 0);
        lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(back_label);

        lv_obj_t* mtitle = lv_label_create(monitor_layer_);
        lv_label_set_text(mtitle, "服务器监控");
        lv_obj_set_style_text_font(mtitle, text_font, 0);
        lv_obj_set_style_text_color(mtitle, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(mtitle, LV_ALIGN_TOP_MID, 0, 14);

        const char* inames[3] = {"CPU", "内存", "磁盘"};
        for (int i = 0; i < 3; i++) {
            int y = 60 + i * 46;
            lv_obj_t* name_l = lv_label_create(monitor_layer_);
            lv_label_set_text(name_l, inames[i]);
            lv_obj_set_style_text_font(name_l, text_font, 0);
            lv_obj_set_style_text_color(name_l, lv_color_hex(0x9DA5B1), 0);
            lv_obj_set_pos(name_l, 20, y);

            lv_obj_t* bar = lv_bar_create(monitor_layer_);
            lv_obj_set_size(bar, 200, 14);
            lv_obj_set_pos(bar, 20, y + 20);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x21262D), LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x2F81F7), LV_PART_INDICATOR);
            mon_bars_[i] = bar;

            lv_obj_t* val_l = lv_label_create(monitor_layer_);
            lv_label_set_text(val_l, "--%");
            lv_obj_set_style_text_font(val_l, text_font, 0);
            lv_obj_set_style_text_color(val_l, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_pos(val_l, 240, y + 14);
            mon_labels_[i] = val_l;
        }

        mon_svc_label_ = lv_label_create(monitor_layer_);
        lv_label_set_text(mon_svc_label_, "服务: --");
        lv_obj_set_style_text_font(mon_svc_label_, text_font, 0);
        lv_obj_set_style_text_color(mon_svc_label_, lv_color_hex(0x9DA5B1), 0);
        lv_obj_set_pos(mon_svc_label_, 20, 205);

        mon_time_label_ = lv_label_create(monitor_layer_);
        lv_label_set_text(mon_time_label_, "刷新: --");
        lv_obj_set_style_text_font(mon_time_label_, text_font, 0);
        lv_obj_set_style_text_color(mon_time_label_, lv_color_hex(0x9DA5B1), 0);
        lv_obj_set_pos(mon_time_label_, 20, 222);

        lv_obj_add_flag(monitor_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(home_layer_);
        ui_mode_ = UiMode::Home;
    }

    void ShowHome() {
        if (home_layer_) { lv_obj_remove_flag(home_layer_, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(home_layer_); }
        if (monitor_layer_) lv_obj_add_flag(monitor_layer_, LV_OBJ_FLAG_HIDDEN);
        ui_mode_ = UiMode::Home;
    }

    void ShowChat() {
        if (home_layer_) lv_obj_add_flag(home_layer_, LV_OBJ_FLAG_HIDDEN);
        if (monitor_layer_) lv_obj_add_flag(monitor_layer_, LV_OBJ_FLAG_HIDDEN);
        ui_mode_ = UiMode::Chat;
    }

    void ShowMonitor() {
        if (home_layer_) lv_obj_add_flag(home_layer_, LV_OBJ_FLAG_HIDDEN);
        if (monitor_layer_) {
            lv_obj_remove_flag(monitor_layer_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(monitor_layer_);
        }
        ui_mode_ = UiMode::Monitor;
        RefreshMonitor();
    }

    void RefreshMonitor() {
        if (monitor_layer_ == nullptr) return;
        auto network = GetNetwork();
        if (network == nullptr) return;
        auto http = network->CreateHttp(0);
        if (http == nullptr) return;
        http->SetHeader("Accept", "application/json");
        if (http->Open("GET", "http://123.56.167.206:8004/")) {
            if (http->GetStatusCode() == 200) {
                std::string body = http->ReadAll();
                http->Close();
                cJSON* root = cJSON_Parse(body.c_str());
                if (root) {
                    DisplayLockGuard lock(display_);
                    auto set_bar = [&](int idx, cJSON* obj, const char* key) {
                        cJSON* v = cJSON_GetObjectItem(obj, key);
                        if (v && cJSON_IsNumber(v)) {
                            int pct = (int)v->valuedouble;
                            if (pct < 0) { pct = 0; }
                            if (pct > 100) { pct = 100; }
                            lv_bar_set_value(mon_bars_[idx], pct, LV_ANIM_ON);
                            char buf[16];
                            snprintf(buf, sizeof(buf), "%d%%", pct);
                            lv_label_set_text(mon_labels_[idx], buf);
                        }
                    };
                    set_bar(0, root, "cpu");
                    set_bar(1, root, "mem_pct");
                    set_bar(2, root, "disk_pct");
                    cJSON* svc = cJSON_GetObjectItem(root, "services");
                    if (svc && cJSON_IsObject(svc)) {
                        std::string s = "服务: ";
                        cJSON* xz = cJSON_GetObjectItem(svc, "xiaozhi");
                        cJSON* oc = cJSON_GetObjectItem(svc, "openclaw");
                        s += (xz && cJSON_IsString(xz) && std::string(xz->valuestring) == "up") ? "小智OK " : "小智XX ";
                        s += (oc && cJSON_IsString(oc) && std::string(oc->valuestring) == "up") ? "OpenClawOK" : "OpenClawXX";
                        lv_label_set_text(mon_svc_label_, s.c_str());
                    }
                    cJSON* t = cJSON_GetObjectItem(root, "time");
                    if (t && cJSON_IsString(t)) {
                        std::string s = "刷新: ";
                        s += t->valuestring;
                        lv_label_set_text(mon_time_label_, s.c_str());
                    }
                    cJSON_Delete(root);
                }
            } else {
                http->Close();
            }
        } else {
            http->Close();
        }
    }

    void HandleHomeTap(lv_coord_t x, lv_coord_t y) {
        if (y < 26) return;
        int col = x / ICON_W;
        int row = (y - 26) / (ICON_H - 10);
        if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) return;
        int idx = row * GRID_COLS + col;
        ESP_LOGI(LAUNCHER_TAG, "Home tap col=%d row=%d idx=%d", col, row, idx);
        switch (idx) {
            case 0: ShowChat(); break;
            case 1: ShowMonitor(); break;
            default: break;
        }
    }

    void HandleMonitorTap(lv_coord_t x, lv_coord_t y) {
        if (x >= 8 && x <= 68 && y >= 8 && y <= 44) {
            ShowHome();
        }
    }

DECLARE_BOARD(FreenoveESP32S3Display);
