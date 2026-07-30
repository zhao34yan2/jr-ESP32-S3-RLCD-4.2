# 📊 RLCD Monitor

基于 **Waveshare ESP32-S3-RLCD-4.2** 开发板的桌面信息监控终端。

4.2" 全反射式单色屏（400×300，1-bit 黑白，无背光），常亮显示天气、黄金、基金、
系统状态等信息。反射屏靠环境光成像，无背光功耗，静止画面几乎零重绘，适合长时间桌面常驻。

---

## 📋 功能总览

| 模块 | 说明 |
|------|------|
| **天气** | 室外温度/天气状况/体感/湿度/风力/降水概率 + 7 天预报（open-meteo，免 Key） |
| **室内温湿度** | 板载 SHTC3 传感器 |
| **黄金价格** | 积存金实时价（东方财富 AU9999，元/克，含日内高/低） |
| **基金监控** | 3 支 QDII 基金净值/涨跌 + 净值日期（东财 FundMNFInfo 官方净值） |
| **DeepSeek 用量** | API 余额（经本地 Bridge 获取） |
| **电池电量** | ADC 检测（16 次采样求平均 + Li-ion 查表），续航趋势估算（纯 RAM，重启重置） |
| **WiFi** | 实时状态，断线 2 分钟自动重连；支持主/备两个网络自动切换 |
| **手机配网** | 连不上 WiFi 时自动开热点 `RLCD-AP`，手机网页填写（Captive Portal 自动弹窗） |
| **夜间省电** | 21:00–07:00 关 WiFi 射频、停 API 请求（CPU 不睡，无唤醒风险） |
| **NTP 时间同步** | 开机同步 + 每 24h 重新对时（中国时区 CST-8） |

---

## 🖥️ 三屏显示（右键循环切换）

设备有三个页面，按**右键（GPIO0 / BOOT）** 循环切换 `第一屏 → 第二屏 → 第三屏 → 第一屏`。

### 第一屏 · 综合信息 (`ui_main.cpp`)

```
┌─ 状态栏 ───────────────────────────────┐
│ 电量 89%       15:02       WiFi:OK 周五 │
├─ 天气 ─────────────────────────────────┤
│ 南京·晴 25-30°C     室内24°C 湿度65% PM35│
├─ 黄金 ─────────────────────────────────┤
│ 黄金 887.90 元/克        低886 高896     │
├─ 基金 ─────────────────────────────────┤
│ 基金   净值日07-24    净值      涨跌     │
│ 摩根日本精选(QDII)A   2.1672   ↑1.81%   │
│ 摩根纳斯达克100(QDII) 1.6955   ↓1.74%   │
│ 广发全球精选(QDII)    6.3974   ↓0.54%   │
├─ DeepSeek ─────────────────────────────┤
│ DeepSeek                    余额 7.46   │
└─────────────────────────────────────────┘
```

### 第二屏 · 天气主题 (`ui_dashboard.cpp`)

大温度 + 天气图标 + 体感/湿度/风力/降水，下方 7 天预报（周几/日期/图标/高低温）。

### 第三屏 · 系统监控 (`ui_sysinfo.cpp`)

2×2 卡片 HUD 风格（完整方框 + 四角括号），白底黑字适配反射屏：

```
┌ 内存 ────────────┐ ┌ 任务数 ──────────┐
│ 5712K            │ │ 13               │
│ / 6144K  USED14% │ │ RUNNING   MAX 30 │
│ ▓▓░░░░░░░░       │ │                  │
├ CPU ─────────────┤ ├ WiFi ────────────┤
│ 160MHz           │ │ ▁▃▅▇ zywifi      │
│ ▓▓▓▓▓▓▓░░░       │ │ -36dBm ch1       │
│ Flash16M PSRAM6M │ │                  │
└──────────────────┘ └──────────────────┘
信息行: IP x.x.x.x  NTP hh:mm  UP hh:mm:ss
```

---

## ⌨️ 按键操作

| 按键 | 操作 | 功能 |
|------|------|------|
| 右键 GPIO0 (BOOT) | 单击 | 循环切屏 |
| 左键 GPIO18 | 短按 (<2s) | 重启网络（关射频再开，触发重连）；夜间省电时忽略 |
| 左键 GPIO18 | 长按 (≥2s) | 重启设备 |

---

## ⚙️ 配置

### 用户凭证 (`firmware/main/secrets.h`)

复制 `secrets.h.example` 为 `secrets.h` 并填入（已被 `.gitignore` 排除，不入仓库）：

| 参数 | 说明 |
|------|------|
| `WIFI_SSID` / `WIFI_PASSWORD` | WiFi 凭证（首次/回退用，日常建议用 AP 配网） |
| `BRIDGE_URL` | Bridge 地址 `http://ip:7777/api/usage`（也可 UDP 自动发现） |
| `WEATHER_LAT` / `WEATHER_LON` | 天气坐标（open-meteo，免 Key） |

> WiFi 凭证优先级：**NVS（AP 配网存入）> secrets.h**。

### 刷新间隔 (`firmware/main/user_config.h`)

| 项目 | 间隔 | 失败重试 |
|------|------|---------|
| 室内温湿度 | 60 秒 | — |
| 天气 | 30 分钟 | 1 分钟 |
| 黄金 | 30 分钟 | 1 分钟 |
| 基金 | 30 分钟 | 1 分钟 |
| DeepSeek | 5 分钟 | 30 秒 |
| 电池采样 | 60 秒 | — |
| UI 循环 | 50ms（内容每 5 秒重算） | — |
| WiFi 重连 | 2 分钟 | — |
| NTP 重对时 | 24 小时 | — |

### 硬件引脚 (`firmware/main/user_config.h`)

| 外设 | 引脚 |
|------|------|
| RLCD MOSI / SCK / DC / CS / RST / TE | 12 / 11 / 5 / 40 / 41 / 6 |
| I2C SDA / SCL（SHTC3 + PCF85063 RTC） | 13 / 14 |
| ADC 电池 | GPIO4 (ADC1_CH3) |
| 右键 / 左键 | GPIO0 (BOOT) / GPIO18 |

---

## 📶 WiFi 配网（AP + Captive Portal）

开机连不上 WiFi 时（约 20 秒超时后），设备自动开热点让手机配网：

1. 手机连开放热点 **`RLCD-AP`**（无密码）；
2. 系统一般自动弹出配置页，否则浏览器访问 `192.168.4.1`；
3. 点「扫描 WiFi」选网络（或手动输名），填密码 → **保存并重启**；
4. 凭证存入 NVS（`cfg` 命名空间），重启后自动连接。

**主/备网络**：换网时旧网络自动降级为备用槽（`ssid2`）。开机先试主网络，
失败自动切备用，不用重新配网。

> ⚠️ ESP32-S3 仅支持 2.4GHz。iPhone 个人热点需开「最大兼容性」。
> SSID 含 `' " < > &` 等特殊字符时请在配网页手动输入。

---

## 🏗️ 项目结构

```
jr-ESP32-S3-RLCD-4.2/
├── firmware/                       # ESP-IDF 固件
│   ├── main/
│   │   ├── main.cpp                # 主入口: 任务调度/按键/配网/省电/电池
│   │   ├── user_config.h           # 硬件引脚 + 刷新间隔
│   │   ├── secrets.h(.example)     # WiFi/Bridge/坐标
│   ├── components/
│   │   ├── net_app/                # WiFi STA/AP + NTP + 射频省电
│   │   ├── sensor/                 # SHTC3 温湿度驱动
│   │   ├── api_clients/            # HTTP 客户端: 天气/黄金/基金/DeepSeek
│   │   ├── ui_app/                 # LVGL 三屏界面
│   │   │   └── fonts/              # 中文字库(GB2312 1bpp) + 数字/天气图标
│   │   ├── app_bsp/                # LVGL BSP
│   │   └── port_bsp/               # ST7305 显示驱动
│   ├── sdkconfig.defaults
│   └── partitions.csv              # 16MB, 单 app 4M 无 OTA
├── bridge/                         # Python Bridge 守护进程
│   └── bridge.py                   # FastAPI 服务 (端口 7777)
└── scripts/                        # 编译/字体/图标生成脚本
```

---

## 🔌 Bridge 守护进程

为设备提供 DeepSeek 用量数据；同时通过 UDP 广播让设备自动发现其地址。

```bash
cd bridge
pip install -r requirements.txt
# 配置 DeepSeek Key (.env 文件或环境变量)
echo "DEEPSEEK_API_KEY=sk-xxxx" > .env
python bridge.py
```

| 端点 | 说明 |
|------|------|
| `GET /api/usage` | DeepSeek 用量（加 `?mock=1` 返回测试数据） |
| `GET /api/fund/{code}` | 基金净值（QDII 回退用） |
| `GET /health` | 健康检查 |

**自动发现**：Bridge 每 5 秒向 `255.255.255.255:7777` 广播 `RLCD_BRIDGE <url>`，
设备监听 UDP:7777 收到后自动写入 NVS，无需手填 `BRIDGE_URL`。

---

## 🚀 编译烧录

```bash
cd firmware
E:\ESP\v5.5.2\esp-idf\export.bat      # 激活 ESP-IDF 环境
idf.py set-target esp32s3             # 首次
idf.py build
idf.py -p COM5 flash monitor          # 烧录 + 串口监控
```

---

## 📦 技术栈

- **ESP-IDF** v5.5.2
- **LVGL** v9.x（1-bit 单色渲染）
- **屏幕** ST7305 400×300 反射式，SPI 接口
- **字体** 等线（DengXian）GB2312 全量 1bpp 中文库 + Montserrat ASCII + 自绘天气图标
- **Bridge** Python 3 + FastAPI + Uvicorn

---

## 💡 设计要点

- **按需刷新**：`set_label` 仅在文本变化时更新，反射屏静止时零重绘（省电、防闪）。
- **数据锁用 mutex**：拷贝大结构体时不长时间关中断，保证 UI 跟手。
- **电池趋势纯 RAM**：不落 NVS，避免碎片化撑爆配网凭证空间；开机清理旧版遗留 NVS。
- **夜间射频省电**：仅关 WiFi radio，不销毁驱动/netif，早晨 `esp_wifi_start` 即恢复，CPU 全程不睡。
