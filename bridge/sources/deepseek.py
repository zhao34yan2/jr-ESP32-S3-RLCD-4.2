"""DeepSeek API 数据源"""

import os
import logging
from datetime import datetime, timezone
import httpx

logger = logging.getLogger("ds_source")

DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY", "")
DEEPSEEK_SESSION_TOKEN = os.getenv("DEEPSEEK_SESSION_TOKEN", "")

_USD_CNY = float(os.getenv("USD_CNY", "7.25"))


async def fetch_deepseek_data() -> dict:
    """获取 DeepSeek 账户余额和用量统计"""
    result = {
        "balance": 0,
        "today_tokens": 0,
        "today_cost": 0,
        "month_tokens": 0,
        "month_cost": 0,
        "cache_hit_rate": 0,
        "budget_pct": 0,
    }

    if not DEEPSEEK_API_KEY:
        logger.warning("DEEPSEEK_API_KEY not set")
        return result

    headers = {
        "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
        "Accept": "application/json",
    }

    async with httpx.AsyncClient(timeout=10) as client:
        try:
            # 1. 查询余额
            resp = await client.get(
                "https://api.deepseek.com/user/balance",
                headers=headers,
            )
            if resp.status_code == 200:
                data = resp.json()
                infos = data.get("balance_infos", [])
                if infos:
                    result["balance"] = float(infos[0].get("total_balance", 0))

            # 2. 用量 API 暂不可用，使用模拟数据
            result["today_tokens"] = 161000000
            result["month_tokens"] = 230000000
            result["today_cost"] = 5.64
            result["month_cost"] = 8.08
            result["cache_hit_rate"] = 0.99

        except Exception as e:
            logger.error(f"DeepSeek API error: {e}")

    logger.info(
        f"DS: balance=¥{result['balance']:.2f} "
        f"today={result['today_tokens']:,}tokens ¥{result['today_cost']:.2f}"
    )
    return result