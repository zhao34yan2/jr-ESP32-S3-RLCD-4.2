#ifndef API_CLIENTS_H
#define API_CLIENTS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 基金信息 */
typedef struct {
    char code[8];            /* 基金代码 */
    char name[48];           /* 基金名称 */
    float nav;               /* 最新净值 */
    float change_pct;        /* 估算涨跌幅 (%) */
    int   is_up;             /* 1=涨, 0=跌 */
} FundItem_t;

#define MAX_FUNDS 3

/* 天气信息 */
#define FORECAST_DAYS 3

typedef struct {
    float temp_outdoor;      /* 室外温度 */
    float temp_min;          /* 最低温 */
    float temp_max;          /* 最高温 */
    char condition[24];      /* 天气状况 (晴/阴/雨) */
    int   pm25;              /* PM2.5 */
    /* 未来几天预报 */
    float fc_min[FORECAST_DAYS];
    float fc_max[FORECAST_DAYS];
    char fc_cond[FORECAST_DAYS][12];
    int  fc_count;
} WeatherData_t;

/* 积存金 (黄金) 信息 — 元/克 */
typedef struct {
    float price;             /* 当前价格 (元/克) */
    float change;            /* 涨跌额 */
    float high;              /* 日内最高 */
    float low;               /* 日内最低 */
    int   is_up;             /* 1=涨, 0=跌 */
} GoldData_t;

/* 白银信息 — 元/千克 */
typedef struct {
    float price;             /* 当前价格 (元/千克) */
    float change;            /* 涨跌额 */
    float high;              /* 日内最高 */
    float low;               /* 日内最低 */
    int   is_up;             /* 1=涨, 0=跌 */
} SilverData_t;

/* DeepSeek 用量 */
typedef struct {
    float balance;           /* 余额 (¥) */
    float today_tokens_m;    /* 今日 tokens (百万) */
    float today_cost;        /* 今日费用 (¥) */
    float month_tokens_m;    /* 本月 tokens (百万) */
    float month_cost;        /* 本月费用 (¥) */
    float cache_hit_rate;    /* 缓存命中率 (0~1) */
    float budget_pct;        /* 预算使用率 (0~1) */
} DeepSeekData_t;

/* 应用全局数据 */
typedef struct {
    /* 传感器 */
    float indoor_temp;       /* 室内温度 */
    float indoor_hum;        /* 室内湿度 */

    /* 天气 */
    WeatherData_t weather;

    /* 积存金 */
    GoldData_t gold;

    /* 白银 */
    SilverData_t silver;

    /* 基金 */
    FundItem_t funds[MAX_FUNDS];
    int   fund_count;
    float total_value;       /* 总市值 */
    float total_change;      /* 总盈亏 */

    /* DeepSeek */
    DeepSeekData_t ds;

    /* 电池 */
    int battery_pct;         /* 0~100 */
    int bat_drop_per_h;      /* 每小时下降% */
    int bat_est_hours;       /* 预估续航小时 */
    int bat_log_count;       /* 历史记录条数 */
    char last_update[10];    /* HH:MM */
} AppData_t;

/**
 * @brief 获取室外天气 (open-meteo, 免 Key)
 */
esp_err_t api_fetch_weather(WeatherData_t *out);

/**
 * @brief 获取积存金价格 (东方财富, AU9999)
 */
esp_err_t api_fetch_gold(GoldData_t *out);

/**
 * @brief 获取白银价格 (东方财富, AG9999)
 */
esp_err_t api_fetch_silver(SilverData_t *out);

/**
 * @brief 获取基金实时估值
 * @param funds 输出基金数组
 * @param count 输出数量
 */
esp_err_t api_fetch_funds(FundItem_t *funds, int *count);

/**
 * @brief 获取 DeepSeek 用量 (通过 bridge)
 */
esp_err_t api_fetch_deepseek(DeepSeekData_t *out);

/**
 * @brief 计算基金总资产摘要
 */
void api_calc_summary(AppData_t *app);

/**
 * @brief 注册默认基金代码列表
 */
void api_set_fund_codes(const char *codes[], int count);

/**
 * @brief 获取 Bridge URL (NVS > 编译默认)
 */
const char *get_bridge_url(void);

/**
 * @brief 设置 Bridge URL (写入 NVS, 永久生效)
 */
void set_bridge_url(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* API_CLIENTS_H */