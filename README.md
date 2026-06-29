# 📊 RLCD Monitor

基于 **Waveshare ESP32-S3-RLCD-4.2** 开发板的桌面信息监控终端。

## 📋 功能总览

- **天气**：室外温度/天气状况（open-meteo API，免 Key）
- **室内温湿度**：板载 SHTC3 传感器
- **黄金价格**：积存金实时价格（新浪期货 nf_AU0）
- **基金监控**：3 支 QDII 基金净值/涨跌（天天基金 API + Bridge 回退）
- **DeepSeek 监控**：API 余额、缓存命中率、Token 消耗（通过本地 Bridge）
- **电池电量**：18650 电池 ADC 检测
- **WiFi 状态**：实时连接状态，断线 10 分钟自动重连
- **NTP 时间同步**：开机自动同步中国时区 CST-8

## 📐 显示布局（5 模块）

```
┌─ ① 状态栏 ─────────────────────────────┐  26px
│ 电量 89%    15:02:38    WiFi:OK 06/25  │
├─ ② 天气 ────────────────────────────────┤  26px
│ ☁ 25~30°C 南京    室内 24°C 湿度 65%RH │
├─ ③ 黄金 ────────────────────────────────┤  26px
│ 积存金 887.90 元/克    低886 高896      │
├─ ④ 基金 ────────────────────────────────┤ 150px
│ 基金          净值      涨跌             │
│ 摩根日本精选  2.2311    --              │
│ 摩根纳斯达克  1.7474   ▼-0.41%          │
│ 广发全球精选  7.6644   ▲0.01%           │
├─ ⑤ DeepSeek ────────────────────────────┤  72px
│ DeepSeek                                 │
│ 余额 7.46            cache 99%           │
│ 今日消耗token161M   本月消耗token230M    │
└──────────────────────────────────────────┘  300px
```

## ⚙️ 配置参数

### 用户配置 (`firmware/main/secrets.h`)

| 参数 | 说明 |
|------|------|
| `WIFI_SSID` / `WIFI_PASSWORD` | WiFi 凭证 |
| `BRIDGE_URL` | Bridge 服务器地址 `http://ip:7777/api/usage` |
| `WEATHER_LAT` / `WEATHER_LON` | 天气坐标 |
| `QWEATHER_API_KEY` | 和风天气 Key（备用） |

### 刷新间隔 (`firmware/main/user_config.h`)

| 项目 | 间隔 | 失败重试 |
|------|------|---------|
| 室内温湿度 | 60 秒 | — |
| 天气 | 30 分钟 | 1 分钟 |
| 黄金 | 30 分钟 | 30 秒 |
| 基金 | 30 分钟 | 30 秒 |
| DeepSeek | 5 分钟 | 30 秒 |
| UI 刷新 | 2 秒 | — |
| WiFi 重连 | 10 分钟 | — |

### 硬件引脚

| 外设 | 引脚 |
|------|------|
| RLCD MOSI/SCK/DC/CS/RST | 12/11/5/40/41 |
| I2C SDA/SCL | 13/14 |
| ADC 电池 | GPIO4 (ADC1_CH3) |

## 🏗️ 项目结构

```
jr-ESP32-S3-RLCD-4.2/
├── firmware/                    # ESP-IDF 固件
│   ├── main/
│   │   ├── main.cpp            # 主入口，任务调度
│   │   ├── user_config.h       # 硬件引脚+软件配置
│   │   ├── secrets.h           # WiFi密码+API Key
│   │   └── secrets.h.example   # 配置模板
│   ├── components/
│   │   ├── net_app/            # WiFi + NTP
│   │   ├── sensor/             # SHTC3 驱动
│   │   ├── api_clients/        # HTTP API 客户端
│   │   ├── ui_app/             # LVGL 界面
│   │   │   └── fonts/          # 自定义字体 (DengXian 14px 1bpp)
│   │   ├── app_bsp/            # LVGL BSP (官方)
│   │   └── port_bsp/           # ST7305 显示驱动 (官方)
│   ├── sdkconfig.defaults      # ESP-IDF 配置
│   └── partitions.csv
├── bridge/                     # Python Bridge 守护进程
│   ├── bridge.py               # FastAPI 服务 (端口 7777)
│   ├── schema.py               # 数据模型
│   ├── sources/deepseek.py     # DeepSeek API 数据源
│   └── requirements.txt
├── scripts/
│   └── _run_build.py           # 编译脚本
└── README.md
```

## 🔌 Bridge 守护进程

DeepSeek 用量 + QDII 基金净值回退。

```bash
cd bridge
pip install -r requirements.txt
export DEEPSEEK_API_KEY="sk-xxxx"
python bridge.py
```

| 端点 | 说明 |
|------|------|
| `GET /api/usage` | DeepSeek 用量 |
| `GET /api/fund/{code}` | 基金净值 |
| `GET /health` | 健康检查 |

## 🚀 编译烧录

```powershell
cd firmware
E:\ESP\v5.5.2\esp-idf\export.bat
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash
```

## 📦 依赖

- **IDF**: ESP-IDF v5.5.2
- **UI**: LVGL v9.5 (1-bit 单色)
- **字体**: DengXian（等线）14px, 1bpp
- **Bridge**: Python 3.13 + FastAPI + Uvicorn
- **屏幕**: 400×300 RLCD 反射式
