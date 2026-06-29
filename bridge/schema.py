"""数据模型"""

from pydantic import BaseModel
from typing import Optional


class DeepSeekUsage(BaseModel):
    balance: float = 0
    today_tokens: int = 0
    today_cost: float = 0
    month_tokens: int = 0
    month_cost: float = 0
    cache_hit_rate: float = 0
    budget_pct: float = 0


class UsageResponse(BaseModel):
    balance: float = 0
    today_tokens: int = 0
    today_cost: float = 0
    month_tokens: int = 0
    month_cost: float = 0
    cache_hit_rate: float = 0
    budget_pct: float = 0
    mock: bool = False