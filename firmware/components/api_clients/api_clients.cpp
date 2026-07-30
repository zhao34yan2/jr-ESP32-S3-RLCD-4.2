/**
 * @file api_clients.cpp
 * @brief HTTP API 客户端: 天气/黄金/基金/DeepSeek
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cJSON.h>
#include "api_clients.h"
#include "secrets.h"
#include "user_config.h"

static const char *TAG = "API";

/* Bridge URL 更新标志 — api_task 轮询此标志, 发现变化立即重拉数据 */
volatile int g_bridge_url_changed = 0;

/* ===== 预置基金列表 (代码 + 默认名, 单一数据源) =====
 * 原来代码在 api 层、中文名在 ui_main.cpp 各存一份, 容易不一致. 合并到这里,
 * UI 经 api_fund_default_name() 取名 (API 返回 SHORTNAME 时优先用返回值). */
typedef struct { const char *code; const char *name; } FundDef_t;
static const FundDef_t s_funds[] = {
    {"007280", "摩根日本精选股票(QDII)A"},
    {"019172", "摩根纳斯达克100指数(QDII)"},
    {"270023", "广发全球精选股票(QDII)"},
};
static const int s_fund_count = (int)(sizeof(s_funds) / sizeof(s_funds[0]));

int api_fund_count(void) { return s_fund_count; }
const char *api_fund_default_name(int i)
{
    return (i >= 0 && i < s_fund_count) ? s_funds[i].name : "";
}

/* ===== HTTP GET over raw socket ===== */
static char *http_get(const char *url)
{
    if (!url) return NULL;
    const char *hp = strstr(url, "://");
    if (!hp) return NULL;
    hp += 3;
    char host[64] = {0}, port_str[8] = "80", path[512] = {0};
    int i = 0;
    while (*hp && *hp != '/' && *hp != ':' && i < 63) host[i++] = *hp++;
    if (*hp == ':') {
        hp++;
        int pi = 0;
        while (*hp && *hp != '/' && pi < 6) port_str[pi++] = *hp++;
        port_str[pi] = '\0';
        if (port_str[0] == '\0') strlcpy(port_str, "80", sizeof(port_str));  /* 畸形 URL ":/": 回填默认端口 */
    }
    if (*hp) strlcpy(path, hp, sizeof(path));
    else strlcpy(path, "/", sizeof(path));

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int dns_ret = getaddrinfo(host, port_str, &hints, &res);
    if (dns_ret != 0 || !res) { ESP_LOGW(TAG, "DNS fail %s", host); return NULL; }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { freeaddrinfo(res); return NULL; }

    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGW(TAG, "connect fail to %s", host);
        close(sock); freeaddrinfo(res); return NULL;
    }
    freeaddrinfo(res);

    char req[768];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ESP\r\nReferer: http://finance.sina.com.cn\r\n\r\n",
        path, host);
    /* snprintf 截断时返回"本应写入长度"(可 >sizeof), 不 clamp 直接 write 会越界读栈. */
    if (req_len < 0) { close(sock); return NULL; }
    if (req_len >= (int)sizeof(req)) req_len = sizeof(req) - 1;
    if (write(sock, req, req_len) != req_len) {
        ESP_LOGW(TAG, "http_get: short write to %s", host);
        close(sock); return NULL;
    }

    /* 动态增长缓冲: 初始 8KB, 不足则翻倍, 上限 64KB (放 PSRAM, 省内部 RAM) */
    size_t cap = 8192;
    char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(cap);   /* PSRAM 不可用时回退内部 RAM */
    if (!buf) { close(sock); return NULL; }
    int total = 0, n;
    /* HTTP/1.0 + Connection close: 服务器读完即关闭, read 返回 0 表示结束 */
    while ((n = read(sock, buf + total, cap - 1 - total)) > 0) {
        total += n;
        if ((size_t)total >= cap - 1) {
            if (cap >= 65536) break;   /* 上限保护 */
            size_t new_cap = cap * 2;
            char *nb = (char *)heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) nb = (char *)realloc(buf, new_cap);
            if (!nb) break;            /* 扩容失败, 用已有数据 */
            buf = nb;
            cap = new_cap;
        }
    }
    buf[total] = '\0';
    close(sock);

    char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        int body_len = total - (body - buf);
        char *result = (char *)heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!result) result = (char *)malloc(body_len + 1);
        if (result) { memcpy(result, body, body_len); result[body_len] = '\0'; free(buf); return result; }
    }
    return buf;
}

/* ===== Weather code → Chinese =====
 * 按 open-meteo WMO 天气码标准映射; 仅用字体已含的汉字
 * (晴/阴/雾/雨/中雨/大雨/雪), 雷雨等归入"雨" */
static const char *code_to_cond(int w)
{
    switch (w) {
        case 0: case 1:                 return "晴";   /* 晴 / 大部晴 */
        case 2: case 3:                 return "阴";   /* 多云 / 阴 */
        case 45: case 48:               return "雾";   /* 雾 / 雾凇 */
        case 51: case 53: case 55:      return "雨";   /* 毛毛雨 */
        case 56: case 57:               return "雨";   /* 冻雨 */
        case 61:                        return "雨";   /* 小雨 */
        case 63:                        return "中雨";
        case 65:                        return "大雨";
        case 66: case 67:               return "雨";   /* 冻雨 */
        case 71: case 73: case 75:      return "雪";
        case 77:                        return "雪";   /* 雪粒 */
        case 80:                        return "雨";   /* 阵雨 */
        case 81:                        return "中雨";
        case 82:                        return "大雨";
        case 85: case 86:               return "雪";   /* 阵雪 */
        case 95: case 96: case 99:      return "雨";   /* 雷雨 → 雨 */
        default:                        return "阴";
    }
}

/* ===== 风速(km/h) → 蒲福风力等级 (0~12) ===== */
static int kmh_to_wind_level(float kmh)
{
    if (kmh < 1)    return 0;
    if (kmh < 6)    return 1;
    if (kmh < 12)   return 2;
    if (kmh < 20)   return 3;
    if (kmh < 29)   return 4;
    if (kmh < 39)   return 5;
    if (kmh < 50)   return 6;
    if (kmh < 62)   return 7;
    if (kmh < 75)   return 8;
    if (kmh < 89)   return 9;
    if (kmh < 103)  return 10;
    if (kmh < 118)  return 11;
    return 12;
}

/* ===== Weather (open-meteo) ===== */
esp_err_t api_fetch_weather(WeatherData_t *out)
{
    char url[640];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
             "weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
             "precipitation_probability_max"
             "&forecast_days=7&timezone=Asia/Shanghai",
             WEATHER_LAT, WEATHER_LON);
    ESP_LOGI(TAG, "Fetching weather: %s", url);

    char *resp = http_get(url);
    if (!resp) return ESP_FAIL;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    cJSON *day = cJSON_GetObjectItem(root, "daily");
    if (cur && day) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(cur, "temperature_2m")))
            out->temp_outdoor = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(cur, "relative_humidity_2m")))
            out->humidity = (int)item->valuedouble;
        if ((item = cJSON_GetObjectItem(cur, "apparent_temperature")))
            out->apparent_temp = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(cur, "wind_speed_10m")))
            out->wind_level = kmh_to_wind_level((float)item->valuedouble);
        if ((item = cJSON_GetObjectItem(cur, "weather_code"))) {
            int w = (int)item->valuedouble;
            out->code = w;
            strlcpy(out->condition, code_to_cond(w), sizeof(out->condition));
        }

        cJSON *tmax = cJSON_GetObjectItem(day, "temperature_2m_max");
        cJSON *tmin = cJSON_GetObjectItem(day, "temperature_2m_min");
        cJSON *pprob = cJSON_GetObjectItem(day, "precipitation_probability_max");
        if (tmax && cJSON_IsArray(tmax) && cJSON_GetArraySize(tmax) > 0)
            out->temp_max = (float)cJSON_GetArrayItem(tmax, 0)->valuedouble;
        if (tmin && cJSON_IsArray(tmin) && cJSON_GetArraySize(tmin) > 0)
            out->temp_min = (float)cJSON_GetArrayItem(tmin, 0)->valuedouble;
        if (pprob && cJSON_IsArray(pprob) && cJSON_GetArraySize(pprob) > 0)
            out->precip_prob = (int)cJSON_GetArrayItem(pprob, 0)->valuedouble;

        /* 多日预报 */
        out->fc_count = 0;
        cJSON *fc_code = cJSON_GetObjectItem(day, "weather_code");
        cJSON *fc_time = cJSON_GetObjectItem(day, "time");
        int fc_sz = 0;
        if (fc_time && cJSON_IsArray(fc_time)) fc_sz = cJSON_GetArraySize(fc_time);
        if (fc_sz > FORECAST_DAYS) fc_sz = FORECAST_DAYS;
        for (int i = 0; i < fc_sz; i++) {
            if (tmin && i < (int)cJSON_GetArraySize(tmin))
                out->fc_min[i] = (float)cJSON_GetArrayItem(tmin, i)->valuedouble;
            if (tmax && i < (int)cJSON_GetArraySize(tmax))
                out->fc_max[i] = (float)cJSON_GetArrayItem(tmax, i)->valuedouble;
            if (fc_code && i < (int)cJSON_GetArraySize(fc_code)) {
                int wc = (int)cJSON_GetArrayItem(fc_code, i)->valuedouble;
                out->fc_code[i] = wc;
                const char *src = code_to_cond(wc);
                /* 手动复制, 避免 strlcpy 潜在问题 */
                int j = 0;
                while (src[j] && j < (int)sizeof(out->fc_cond[i]) - 1) {
                    out->fc_cond[i][j] = src[j];
                    j++;
                }
                out->fc_cond[i][j] = '\0';
            }
            out->fc_count++;
        }

        out->pm25 = 0;
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Weather: %.1fC %s hum%d%% feel%.1f wind%d",
                 out->temp_outdoor, out->condition, out->humidity,
                 out->apparent_temp, out->wind_level);
        return ESP_OK;
    }
    cJSON_Delete(root);
    return ESP_FAIL;
}

/* ===== Gold (Sina futures nf_AU0) ===== */
esp_err_t api_fetch_gold(GoldData_t *out)
{
    char *resp = http_get("http://hq.sinajs.cn/list=nf_AU0");
    if (!resp) return ESP_FAIL;

    char *p = strchr(resp, '"');
    if (!p) { free(resp); return ESP_FAIL; }
    p++;
    char *end = strchr(p, '"');
    if (!end) { free(resp); return ESP_FAIL; }
    *end = '\0';

    /* 先解析到局部变量, 全部有效再写回 out — 避免非交易时段(Sina 返回空串或 0)
     * 把 out 的旧值和错误涨跌混在一起当成功上报 */
    int field = 0;
    float prev_close = 0, price = 0, high = 0, low = 0;
    char *tok = p;
    while (tok && *tok) {
        char *comma = strchr(tok, ',');
        if (comma) *comma++ = '\0';
        char *val = tok;
        while (*val == ' ') val++;
        switch (field) {
            case 2: prev_close = (float)atof(val); break;  /* 昨收盘 */
            case 3: { float v = (float)atof(val); if (v > 0.001f) high  = v; break; }
            case 4: { float v = (float)atof(val); if (v > 0.001f) low   = v; break; }
            case 6: { float v = (float)atof(val); if (v > 0.001f) price = v; break; }
        }
        tok = comma;
        field++;
    }
    free(resp);

    /* price 未拿到 → 本次无效, 返回 FAIL 让调用方保留上一次好数据 */
    if (price <= 0.001f) {
        ESP_LOGW(TAG, "Gold: no valid price (market closed?), keep old");
        return ESP_FAIL;
    }
    out->price = price;
    if (high > 0.001f) out->high = high;
    if (low  > 0.001f) out->low  = low;
    /* 仅在昨收盘也有效时才算涨跌; 否则不动旧涨跌, 避免出现 "涨跌额=整个价格" */
    if (prev_close > 0.001f) {
        out->change = price - prev_close;
        out->is_up = (out->change >= 0) ? 1 : 0;
    }
    ESP_LOGI(TAG, "Gold: %.2f h%.2f l%.2f", out->price, out->high, out->low);
    return ESP_OK;
}

/* ===== HTTPS GET using esp_http_client (支持自定义头) ===== */
static char *https_get(const char *url, const char *header_key, const char *header_val)
{
    esp_http_client_config_t cfg = {0};
    cfg.url = url; cfg.timeout_ms = 10000; cfg.buffer_size = 16384;
    cfg.skip_cert_common_name_check = true;
    ESP_LOGI(TAG, "HTTPS connecting: %.50s", url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    if (header_key && header_val) {
        esp_http_client_set_header(client, header_key, header_val);
    }
    /* 16KB 缓冲放 PSRAM, 避免反复占用内部 RAM 造成碎片 */
    char *buf = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(16384);   /* PSRAM 不可用时回退内部 RAM */
    if (!buf) { esp_http_client_cleanup(client); return NULL; }
    memset(buf, 0, 16384);
    esp_err_t err = esp_http_client_open(client, 0);
    bool read_err = false;
    if (err == ESP_OK) {
        int cl = esp_http_client_fetch_headers(client);
        int total = 0, n = 0;
        while (total < 16383 && (n = esp_http_client_read(client, buf + total, 16383 - total)) > 0) {
            total += n;
        }
        buf[total] = '\0';
        if (n < 0) {
            /* TLS 中途出错: 得到半截 body, 解析必失败, 按失败处理而非误报成功 */
            read_err = true;
            ESP_LOGW(TAG, "HTTPS read error mid-stream: %s (%d bytes so far)", url, total);
        } else if (total >= 16383) {
            /* 触顶截断: body 超 16KB, 剩余被丢弃, JSON 多半不完整 */
            ESP_LOGW(TAG, "HTTPS truncated at 16KB: %s (cl=%d)", url, cl);
        } else {
            ESP_LOGI(TAG, "HTTPS OK: %s (%d bytes, cl=%d)", url, total, cl);
        }
    } else {
        ESP_LOGW(TAG, "HTTPS fail: %s (%s)", url, esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || read_err || buf[0] == '\0') {
        ESP_LOGW(TAG, "HTTPS no data: %s", url);
        free(buf);
        return NULL;
    }
    return buf;
}

static void get_bridge_base(char *buf, size_t sz);

/* ===== Funds (fundgz + East Money HTTPS fallback) ===== */
/* 单支基金走 Bridge 兜底 (东财主接口失败时) */
static bool fund_fetch_bridge(FundItem_t *f, const char *code)
{
    char base[64];
    get_bridge_base(base, sizeof(base));
    char burl[256];
    snprintf(burl, sizeof(burl), "%s/api/fund/%s", base, code);
    char *bp = http_get(burl);
    if (!bp) { ESP_LOGW(TAG, "Fund %s: Bridge fail", code); return false; }
    ESP_LOGI(TAG, "Bridge resp: %.60s", bp);
    cJSON *br = cJSON_Parse(bp); free(bp);
    if (!br) { ESP_LOGW(TAG, "Bridge: JSON parse fail"); return false; }
    bool ok = false;
    cJSON *item = cJSON_GetObjectItem(br, "nav");
    if (item) { f->nav = (float)item->valuedouble; ok = true; }
    else ESP_LOGW(TAG, "Bridge: no nav field");
    cJSON_Delete(br);
    return ok;
}

/* ===== Funds — 东财移动端 FundMNFInfo (一次请求查全部) =====
 * fundgz 实时估值接口已下线; QDII 本无实时估值 (GSZ=null).
 * 改用官方单位净值 NAV + 净值日期 PDATE + 官方涨跌 NAVCHGRT, 一次请求查多支.
 * 单支失败回退 Bridge. */
esp_err_t api_fetch_funds(FundItem_t *funds, int *count)
{
    *count = s_fund_count;

    /* 预置代码, 并记录哪些已成功 (供 Bridge 兜底判断) */
    bool ok[MAX_FUNDS] = { false };
    for (int i = 0; i < s_fund_count && i < MAX_FUNDS; i++)
        strlcpy(funds[i].code, s_funds[i].code, sizeof(funds[i].code));

    /* 拼多基金查询: Fcodes=code1,code2,... */
    char codes[64] = {0};
    for (int i = 0; i < s_fund_count && i < MAX_FUNDS; i++) {
        strlcat(codes, s_funds[i].code, sizeof(codes));
        if (i + 1 < s_fund_count) strlcat(codes, ",", sizeof(codes));
    }
    char url[256];
    snprintf(url, sizeof(url),
             "https://fundmobapi.eastmoney.com/FundMNewApi/FundMNFInfo"
             "?pageIndex=1&pageSize=%d&plat=Android&appType=ttjj&product=EFund"
             "&Version=1&deviceid=esp32&Fcodes=%s",
             s_fund_count, codes);
    ESP_LOGI(TAG, "Funds FundMNFInfo: %s", codes);

    char *resp = https_get(url, "Referer", "https://fund.eastmoney.com/");
    int got = 0;
    if (resp) {
        cJSON *root = cJSON_Parse(resp);
        free(resp);
        if (root) {
            cJSON *datas = cJSON_GetObjectItem(root, "Datas");
            if (datas && cJSON_IsArray(datas)) {
                int n = cJSON_GetArraySize(datas);
                for (int k = 0; k < n; k++) {
                    cJSON *d = cJSON_GetArrayItem(datas, k);
                    cJSON *jc = cJSON_GetObjectItem(d, "FCODE");
                    if (!jc || !cJSON_IsString(jc)) continue;
                    /* 找到对应槽位 */
                    int idx = -1;
                    for (int i = 0; i < s_fund_count && i < MAX_FUNDS; i++)
                        if (strcmp(funds[i].code, jc->valuestring) == 0) { idx = i; break; }
                    if (idx < 0) continue;
                    FundItem_t *f = &funds[idx];
                    cJSON *it;
                    if ((it = cJSON_GetObjectItem(d, "SHORTNAME")) && cJSON_IsString(it))
                        strlcpy(f->name, it->valuestring, sizeof(f->name));
                    if ((it = cJSON_GetObjectItem(d, "NAV")) && cJSON_IsString(it) && it->valuestring[0])
                        f->nav = (float)atof(it->valuestring);
                    if ((it = cJSON_GetObjectItem(d, "NAVCHGRT")) && cJSON_IsString(it) && it->valuestring[0]) {
                        f->change_pct = (float)atof(it->valuestring);
                        f->is_up = (f->change_pct >= 0) ? 1 : 0;
                    }
                    /* PDATE 形如 "2026-07-22", 存成 "07-22" 省显示空间 */
                    if ((it = cJSON_GetObjectItem(d, "PDATE")) && cJSON_IsString(it) &&
                        strlen(it->valuestring) >= 10)
                        strlcpy(f->nav_date, it->valuestring + 5, sizeof(f->nav_date));
                    if (f->nav > 0.001f) { ok[idx] = true; got++;
                        ESP_LOGI(TAG, "Fund %s: %.4f (%s) %.2f%%", f->code, f->nav, f->nav_date, f->change_pct); }
                }
            }
            cJSON_Delete(root);
        }
    }

    /* 未成功的基金逐个走 Bridge 兜底 */
    for (int i = 0; i < s_fund_count && i < MAX_FUNDS; i++) {
        if (ok[i] || funds[i].nav > 0.001f) continue;
        if (fund_fetch_bridge(&funds[i], s_funds[i].code)) got++;
    }

    return (got > 0) ? ESP_OK : ESP_FAIL;
}

/* ===== Bridge URL 管理 (NVS 持久化, 免重新编译) =====
 * discovery_task 写 (set_bridge_url), api_task 读 (get_bridge_base / DeepSeek 回退),
 * 两任务并发. s_bridge_url 是 128B 数组, strlcpy 非原子, 无锁会读到半新半旧的撕裂串.
 * 用 mutex 串行化; 并发读路径改用 get_bridge_url_copy 在锁内拷到本地, 不再持逃逸指针. */
static char s_bridge_url[128] = "";
static SemaphoreHandle_t s_bridge_mutex = NULL;
#define BRIDGE_LOCK()   do { if (s_bridge_mutex) xSemaphoreTake(s_bridge_mutex, portMAX_DELAY); } while (0)
#define BRIDGE_UNLOCK() do { if (s_bridge_mutex) xSemaphoreGive(s_bridge_mutex); } while (0)

const char *get_bridge_url(void)
{
    /* 首次调用发生在 app_main 启动早期 (单线程), 此时懒建锁无竞态 */
    if (!s_bridge_mutex) s_bridge_mutex = xSemaphoreCreateMutex();
    BRIDGE_LOCK();
    if (s_bridge_url[0] == '\0') {
        nvs_handle_t nvs;
        esp_err_t err = nvs_open("bridge", NVS_READONLY, &nvs);
        if (err == ESP_OK) {
            size_t sz = sizeof(s_bridge_url);
            if (nvs_get_str(nvs, "url", s_bridge_url, &sz) != ESP_OK) {
                strlcpy(s_bridge_url, BRIDGE_URL, sizeof(s_bridge_url));
            }
            nvs_close(nvs);
        } else {
            strlcpy(s_bridge_url, BRIDGE_URL, sizeof(s_bridge_url));
        }
    }
    BRIDGE_UNLOCK();
    return s_bridge_url;
}

/* 锁内把当前 Bridge URL 拷到调用方缓冲, 供并发任务安全读取 */
static void get_bridge_url_copy(char *buf, size_t sz)
{
    if (!buf || sz == 0) return;
    get_bridge_url();          /* 确保已懒加载 + 锁已建 */
    BRIDGE_LOCK();
    strlcpy(buf, s_bridge_url, sz);
    BRIDGE_UNLOCK();
}

static void get_bridge_base(char *buf, size_t sz)
{
    char url[128];
    get_bridge_url_copy(url, sizeof(url));   /* 锁内拷本地, 避免与 set_bridge_url 竞争 */
    const char *s = strstr(url, "://");
    if (!s) { strlcpy(buf, url, sz); return; }
    s += 3;
    const char *e = strchr(s, '/');
    if (!e) { strlcpy(buf, url, sz); return; }
    size_t base_len = e - url;
    if (base_len >= sz) base_len = sz - 1;
    memcpy(buf, url, base_len);
    buf[base_len] = '\0';
}

void set_bridge_url(const char *url)
{
    nvs_handle_t nvs;
    if (nvs_open("bridge", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "url", url);
        nvs_commit(nvs);
        nvs_close(nvs);
        if (!s_bridge_mutex) s_bridge_mutex = xSemaphoreCreateMutex();
        BRIDGE_LOCK();
        strlcpy(s_bridge_url, url, sizeof(s_bridge_url));
        BRIDGE_UNLOCK();
        g_bridge_url_changed = 1;
        ESP_LOGI(TAG, "Bridge URL updated: %s", url);
    }
}

/* ===== DeepSeek: 直连 HTTPS, 回退 Bridge ===== */
esp_err_t api_fetch_deepseek(DeepSeekData_t *out)
{
    /* 尝试直连 DeepSeek API */
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", DEEPSEEK_API_KEY);
    char *resp = https_get("https://api.deepseek.com/user/balance", "Authorization", auth);
    if (resp) {
        cJSON *root = cJSON_Parse(resp); free(resp);
        if (root) {
            /* 只有确实解析到 total_balance 才算成功; 否则 fall through 到 Bridge 兜底.
             * (接口返回合法 JSON 但结构异常/错误体时, 不能报成功污染旧值) */
            bool got = false;
            cJSON *infos = cJSON_GetObjectItem(root, "balance_infos");
            if (infos && cJSON_IsArray(infos) && cJSON_GetArraySize(infos) > 0) {
                cJSON *info = cJSON_GetArrayItem(infos, 0);
                cJSON *tb = cJSON_GetObjectItem(info, "total_balance");
                if (tb && tb->valuestring) { out->balance = (float)atof(tb->valuestring); got = true; }
            }
            cJSON_Delete(root);
            if (got) {
                ESP_LOGI(TAG, "DS direct: %.2f", out->balance);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "DS direct: no total_balance, fallback to bridge");
        }
    }

    /* 回退 Bridge */
    char bridge_url[128];
    get_bridge_url_copy(bridge_url, sizeof(bridge_url));
    resp = http_get(bridge_url);
    if (!resp) return ESP_FAIL;
    cJSON *root = cJSON_Parse(resp); free(resp);
    if (!root) return ESP_FAIL;
    cJSON *i;
    if ((i = cJSON_GetObjectItem(root, "balance")))       out->balance      = (float)i->valuedouble;
    if ((i = cJSON_GetObjectItem(root, "today_tokens")))  out->today_tokens_m = (float)i->valuedouble / 1e6f;
    if ((i = cJSON_GetObjectItem(root, "today_cost")))    out->today_cost    = (float)i->valuedouble;
    if ((i = cJSON_GetObjectItem(root, "month_tokens")))  out->month_tokens_m = (float)i->valuedouble / 1e6f;
    if ((i = cJSON_GetObjectItem(root, "month_cost")))    out->month_cost   = (float)i->valuedouble;
    if ((i = cJSON_GetObjectItem(root, "cache_hit_rate")))out->cache_hit_rate = (float)i->valuedouble;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "DS bridge: %.2f", out->balance);
    return ESP_OK;
}

