# Qoder 任务书：freenove-esp32s3-display-2.8-lcd 固件三改

工作目录：/root/xiaozhi-esp32（git 仓库，改完要 push 到 fork 触发 GitHub Actions 编译）

## 需求（用户钦定）
1. **竖屏**：这块屏是 2.8 寸 240x320 ILI9341，当前横屏（320x240 + SWAP_XY true）。改成竖屏。
2. **电量显示百分比**：电池图标旁显示百分比数字（如 "63%"）。
3. **DeepSeek 余额显示在屏幕右下角**，每 10 秒刷新一次（数据来自服务器，固件轮询 HTTP 接口）。

## 具体改动点（已探明）

### A. 竖屏 — main/boards/freenove-esp32s3-display-2.8-lcd/config.h
```
#define DISPLAY_WIDTH         320   → 240
#define DISPLAY_HEIGHT        240   → 320
#define DISPLAY_SWAP_XY       true  → false（屏原生竖排 240x320）
```
同时检查 main/display/lcd_display.cc 的 UI 布局是否在竖屏下正常（top_bar 横贯、status 居中、chat 区自适应），需要适配就一起改。

### B. 电量百分比 — main/display/lcd_display.cc（或 lvgl_display.cc）
- SetupUI 里 battery_label_ 只显示图标，新增一个百分比文本 label（如 "63%"），放在电池图标旁边
- UpdateStatusBar() 里已经有 `battery_level` 变量，把数字格式化写进百分比 label
- 注意竖屏后顶部状态栏空间，别挤爆

### C. DeepSeek 余额 — 右下角 + 每 10 秒刷新
- 新增一个 label 固定在屏幕右下角（LV_ALIGN_BOTTOM_RIGHT，留边距）
- 固件每 10 秒 HTTP GET 一次余额接口（服务器地址：http://<服务器IP>:8003 或 8000，接口路径待定，先用 http://192.168.0.8:8003/balance 这种可配置形式，实际地址做成宏/可配置）
- 拿到 JSON 显示 "余额 ¥xx.xx"；拿不到显示 "--"
- 不要阻塞主线程，用定时器/异步任务

### D. 服务器侧余额接口（同仓库不同目录，供固件轮询）
服务器代码在 /root/xiaozhi-esp32-server/main/xiaozhi-server/（Python aiohttp）
- 在 core/http_server.py 或新增 handler 加一个 GET 接口返回 DeepSeek 余额
- DeepSeek 余额 API：GET https://api.deepseek.com/user/balance，Header: Authorization: Bearer <DEEPSEEK_API_KEY>
- 服务器缓存余额（每 10 秒查一次 DeepSeek，避免被打爆）
- 返回 JSON：{"balance": 12.34, "currency": "CNY"}

## 红线
- 不动其他板型的代码
- 不破坏现有对话/语音/屏幕表情功能
- 改动最小化，注释清晰
- 竖屏后触摸坐标、聊天气泡方向要跟着适配

## 验证标准
- 编译能过（GitHub Actions 会自动验证，push 后看结果）
- 电量百分比随电池变化刷新
- 右下角余额每 10 秒刷新
- 竖屏显示正常

请开始干活，改完告诉我改了哪些文件、每个文件的要点，以及你建议的服务器余额接口路径。
