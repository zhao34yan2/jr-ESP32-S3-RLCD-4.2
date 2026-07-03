"""
RLCD Monitor Bridge — 守护进程

提供 DeepSeek 用量数据给 ESP32-S3-RLCD-4.2 设备端。
数据缓存在内存中，定期刷新。

启动:
    python bridge.py
    # 或带 mock 数据测试:
    curl http://localhost:7777/api/usage?mock=1
"""

import os
import sys
import json
import socket
import struct
import asyncio
import logging
import threading
from contextlib import asynccontextmanager
from datetime import datetime

from dotenv import load_dotenv

# ⚠️ 必须先加载 .env, 再 import deepseek (否则环境变量为空)
load_dotenv()

from fastapi import FastAPI, Query, HTTPException
from fastapi.middleware.cors import CORSMiddleware
import uvicorn

from schema import UsageResponse
from sources.deepseek import fetch_deepseek_data, DEEPSEEK_API_KEY

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
)
logger = logging.getLogger("bridge")

# 缓存
_cache = {"data": {}, "updated_at": None}
_lock = asyncio.Lock()
_POLL_SEC = int(os.getenv("RLCD_POLL_SEC", "60"))


def _mock_data() -> dict:
    """生成模拟测试数据"""
    return {
        "balance": 37.42,
        "today_tokens": 161_000_000,
        "today_cost": 5.64,
        "month_tokens": 230_000_000,
        "month_cost": 8.08,
        "cache_hit_rate": 0.99,
        "budget_pct": 0.60,
    }


async def _refresh_cache():
    """后台刷新缓存"""
    while True:
        try:
            data = await fetch_deepseek_data()
            async with _lock:
                _cache["data"] = data
                _cache["updated_at"] = datetime.now().isoformat()
            logger.info("Cache refreshed")
        except Exception as e:
            logger.error(f"Cache refresh error: {e}")
        await asyncio.sleep(_POLL_SEC)


def _udp_broadcast():
    """UDP 广播：让 ESP32 自动发现 Bridge 地址"""
    host = os.getenv("RLCD_HOST", "0.0.0.0")
    port = int(os.getenv("RLCD_PORT", "7777"))
    # 获取本机实际 IP
    local_ip = "127.0.0.1"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except:
        pass

    msg = f"RLCD_BRIDGE http://{local_ip}:{port}/api/usage"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        try:
            sock.sendto(msg.encode(), ("255.255.255.255", 7777))
            threading.Event().wait(5)  # 每5秒广播一次
        except:
            threading.Event().wait(5)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """启动/关闭生命周期"""
    # 启动
    bg_task = asyncio.create_task(_refresh_cache())
    udp_thread = threading.Thread(target=_udp_broadcast, daemon=True)
    udp_thread.start()
    if DEEPSEEK_API_KEY:
        logger.info("DeepSeek API key configured ✓")
    else:
        logger.info("DeepSeek API key not set, using mock data")
    logger.info(f"Bridge started, poll interval: {_POLL_SEC}s")
    yield
    # 关闭
    bg_task.cancel()
    logger.info("Bridge stopped")


app = FastAPI(title="RLCD Monitor Bridge", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/api/usage", response_model=UsageResponse)
async def get_usage(mock: bool = Query(False)):
    """获取 DeepSeek 用量数据"""
    if mock or not DEEPSEEK_API_KEY:
        if not DEEPSEEK_API_KEY:
            logger.info("No API key, using mock data")
        return UsageResponse(**_mock_data(), mock=True)

    async with _lock:
        if not _cache["data"]:
            return UsageResponse(**_mock_data(), mock=True)
        return UsageResponse(**_cache["data"])


@app.get("/api/fund/{code}")
async def get_fund(code: str):
    """获取基金净值 (用于 QDII 等无实时估值的基金)"""
    import httpx
    url = f"https://fund.eastmoney.com/pingzhongdata/{code}.js"
    try:
        async with httpx.AsyncClient(timeout=10) as client:
            resp = await client.get(url)
            text = resp.text
            # 提取最新净值: Data_netWorthTrend = [{...},...,{"x":...,"y":1.2345,...}]
            import json
            # 手动找匹配的括号: Data_netWorthTrend = [ ... ];
            start = text.find('Data_netWorthTrend = [')
            if start >= 0:
                start += len('Data_netWorthTrend = [')
                depth = 1
                i = start
                while i < len(text) and depth > 0:
                    if text[i] == '[': depth += 1
                    elif text[i] == ']': depth -= 1
                    i += 1
                json_str = text[start:i-1]
                data = json.loads('[' + json_str + ']')
                if data:
                    nav = float(data[-1]['y'])
                    logger.info(f"Fund {code} NAV: {nav}")
                    return {"code": code, "nav": nav}
    except Exception as e:
        logger.error(f"Fund {code} error: {e}")
    return {"code": code, "nav": 0}


@app.get("/health")
async def health():
    return {"status": "ok", "updated_at": _cache.get("updated_at")}


# ===== 电池电量日志 (ESP32 定期上报) =====
_bat_log = []  # [{ts, pct}, ...]

@app.post("/api/battery/upload")
async def battery_upload(data: dict):
    """ESP32 上报电池数据"""
    _bat_log.append({"ts": data.get("ts"), "pct": data.get("pct")})
    # 只保留最近 24 小时
    cutoff = datetime.now().timestamp() - 86400
    _bat_log[:] = [r for r in _bat_log if r["ts"] and r["ts"] > cutoff]
    return {"ok": True, "count": len(_bat_log)}

@app.get("/api/battery")
async def battery_get():
    """获取电池历史数据"""
    latest = _bat_log[-1] if _bat_log else {"pct": 0, "ts": 0}
    drop = 0
    if len(_bat_log) >= 2:
        first = _bat_log[0]
        elapsed_h = (latest["ts"] - first["ts"]) / 3600 if latest["ts"] > first["ts"] else 0
        if elapsed_h > 0.1:
            drop = (first["pct"] - latest["pct"]) / elapsed_h
    return {
        "records": _bat_log[-288:],  # 最多返回最近 24h
        "summary": {
            "current": latest["pct"],
            "drop_per_h": round(drop, 1),
            "total_records": len(_bat_log),
        }
    }


def main():
    host = os.getenv("RLCD_HOST", "0.0.0.0")
    port = int(os.getenv("RLCD_PORT", "7777"))
    uvicorn.run(app, host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
