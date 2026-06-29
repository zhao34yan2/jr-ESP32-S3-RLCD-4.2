"""天气数据源 — open-meteo（免 API Key）"""

import os
import logging
import httpx

logger = logging.getLogger("weather_source")

_LAT = os.getenv("WEATHER_LAT", "31.2304")
_LON = os.getenv("WEATHER_LON", "121.4737")


async def fetch_weather() -> dict:
    """获取室外天气"""
    url = (
        f"https://api.open-meteo.com/v1/forecast"
        f"?latitude={_LAT}&longitude={_LON}"
        f"&current=temperature_2m,weather_code"
        f"&daily=temperature_2m_max,temperature_2m_min"
        f"&timezone=auto"
    )

    async with httpx.AsyncClient(timeout=8) as client:
        try:
            resp = await client.get(url)
            if resp.status_code != 200:
                logger.warning(f"Weather API returned {resp.status_code}")
                return {}
            return resp.json()
        except Exception as e:
            logger.error(f"Weather fetch error: {e}")
            return {}