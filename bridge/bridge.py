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
import asyncio
import logging
from datetime import datetime

from dotenv import load_dotenv
from fastapi import FastAPI, Query, HTTPException
from fastapi.middleware.cors import CORSMiddleware
import uvicorn

from schema import UsageResponse
from sources.deepseek import fetch_deepseek_data, DEEPSEEK_API_KEY

# 加载 .env
load_dotenv()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
)
logger = logging.getLogger("bridge")

app = FastAPI(title="RLCD Monitor Bridge")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# 缓存
_cache = {
    "data": {},
    "updated_at": None,
}
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


@app.on_event("startup")
async def startup():
    asyncio.create_task(_refresh_cache())
    logger.info("Bridge started")
    logger.info(f"Poll interval: {_POLL_SEC}s")


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
            import re, json
            m = re.search(r'Data_netWorthTrend\s*=\s*(\[.*?\]);', text, re.DOTALL)
            if m:
                data = json.loads(m.group(1))
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


def main():
    host = os.getenv("RLCD_HOST", "0.0.0.0")
    port = int(os.getenv("RLCD_PORT", "7777"))
    uvicorn.run(app, host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
