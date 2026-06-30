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
#include <cJSON.h>
#include "api_clients.h"
#include "secrets.h"
#include "user_config.h"

static const char *TAG = "API";

/* ===== Default fund codes ===== */
static const char *s_fund_codes[] = {
    "007280",  /* 摩根日本精选股票(QDII)A */
    "019172",  /* 摩根纳斯达克100指数(QDII)人民币A */
    "270023",  /* 广发全球精选股票(QDII)人民币A */
};
static int s_fund_count = 3;

void api_set_fund_codes(const char *codes[], int count)
{
    if (count > MAX_FUNDS) count = MAX_FUNDS;
    s_fund_count = count;
    for (int i = 0; i < count; i++) s_fund_codes[i] = codes[i];
}

/* ===== HTTP GET over raw socket ===== */
static char *http_get(const char *url)
{
    if (!url) return NULL;
    const char *hp = strstr(url, "://");
    if (!hp) return NULL;
    hp += 3;
    char host[64] = {0}, port_str[8] = "80", path[256] = {0};
    int i = 0;
    while (*hp && *hp != '/' && *hp != ':' && i < 63) host[i++] = *hp++;
    if (*hp == ':') {
        hp++;
        int pi = 0;
        while (*hp && *hp != '/' && pi < 6) port_str[pi++] = *hp++;
        port_str[pi] = '\0';
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

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ESP\r\nReferer: http://finance.sina.com.cn\r\n\r\n",
        path, host);
    write(sock, req, req_len);

    char *buf = (char *)malloc(4096);
    if (!buf) { close(sock); return NULL; }
    memset(buf, 0, 4096);
    int total = 0, n;
    while (total < 4095 && (n = read(sock, buf + total, 4095 - total)) > 0) {
        total += n;
        if (total >= 7 && memcmp(buf + total - 5, "\r\n0\r\n", 4) == 0) break;
    }
    buf[total] = '\0';
    close(sock);

    char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        int body_len = total - (body - buf);
        char *result = (char *)malloc(body_len + 1);
        if (result) { memcpy(result, body, body_len); result[body_len] = '\0'; free(buf); return result; }
    }
    return buf;
}

/* ===== Weather code → Chinese ===== */
static const char *code_to_cond(int w)
{
    if (w <= 3) return "晴";
    if (w <= 20) return "阴";
    if (w <= 50) return "雾";
    if (w <= 60) return "小雨";
    if (w <= 70) return "中雨";
    if (w <= 80) return "大雨";
    if (w <= 86) return "雪";
    return "雨";
}

/* ===== Weather (open-meteo) ===== */
esp_err_t api_fetch_weather(WeatherData_t *out)
{
    char url[512];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=Asia/Shanghai",
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
        if ((item = cJSON_GetObjectItem(cur, "weather_code"))) {
            int w = (int)item->valuedouble;
            strcpy(out->condition, code_to_cond(w));
        }

        cJSON *tmax = cJSON_GetObjectItem(day, "temperature_2m_max");
        cJSON *tmin = cJSON_GetObjectItem(day, "temperature_2m_min");
        if (tmax && cJSON_IsArray(tmax) && cJSON_GetArraySize(tmax) > 0)
            out->temp_max = (float)cJSON_GetArrayItem(tmax, 0)->valuedouble;
        if (tmin && cJSON_IsArray(tmin) && cJSON_GetArraySize(tmin) > 0)
            out->temp_min = (float)cJSON_GetArrayItem(tmin, 0)->valuedouble;

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
        ESP_LOGI(TAG, "Weather: %.1fC %s", out->temp_outdoor, out->condition);
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

    int field = 0;
    char *tok = p;
    while (tok && *tok) {
        char *comma = strchr(tok, ',');
        if (comma) *comma++ = '\0';
        char *val = tok;
        while (*val == ' ') val++;
        switch (field) {
            case 2: { float pv = (float)atof(val); out->change = out->price - pv; break; }
            case 3: { float v = (float)atof(val); if (v > 0.001f) out->high  = v; break; }
            case 4: { float v = (float)atof(val); if (v > 0.001f) out->low   = v; break; }
            case 6: { float v = (float)atof(val); if (v > 0.001f) out->price = v; break; }
        }
        tok = comma;
        field++;
    }
    out->is_up = (out->change >= 0) ? 1 : 0;
    free(resp);
    ESP_LOGI(TAG, "Gold: %.2f h%.2f l%.2f", out->price, out->high, out->low);
    return ESP_OK;
}

/* ===== Silver stub ===== */
esp_err_t api_fetch_silver(SilverData_t *out) { return ESP_FAIL; }

/* ===== Funds (fundgz + bridge fallback) ===== */
esp_err_t api_fetch_funds(FundItem_t *funds, int *count)
{
    int got = 0;
    for (int i = 0; i < s_fund_count && i < MAX_FUNDS; i++) {
        FundItem_t *f = &funds[i];

        /* 保留旧代码, 只更新成功获取的数据 */
        strlcpy(f->code, s_fund_codes[i], sizeof(f->code));

        /* 尝试 fundgz 实时估值 */
        char url[128];
        snprintf(url, sizeof(url), "http://fundgz.1234567.com.cn/js/%s.js", s_fund_codes[i]);
        char *resp = http_get(url);
        if (resp) {
            char *p = strchr(resp, '{');
            if (p) {
                char *end = strrchr(resp, '}');
                if (end) {
                    *(end + 1) = '\0';
                    cJSON *root = cJSON_Parse(p);
                    if (root) {
                        cJSON *item;
                        if ((item = cJSON_GetObjectItem(root, "name")))
                            strlcpy(f->name, item->valuestring, sizeof(f->name));
                        if ((item = cJSON_GetObjectItem(root, "gsz")))
                            f->nav = (float)atof(item->valuestring);
                        if ((item = cJSON_GetObjectItem(root, "gszzl"))) {
                            f->change_pct = (float)atof(item->valuestring);
                            f->is_up = (f->change_pct >= 0) ? 1 : 0;
                        }
                        if (f->nav < 0.001f && (item = cJSON_GetObjectItem(root, "dwjz")))
                            f->nav = (float)atof(item->valuestring);
                        cJSON_Delete(root);
                        free(resp);
                        ESP_LOGI(TAG, "Fund %s: %.4f", f->code, f->nav);
                        got++;
                        continue;
                    }
                }
            }
            free(resp);
        }

        /* Fallback to bridge (保留旧数据不覆盖) */
        char bhost[64] = {0}, bport[8] = "80";
        const char *s = strstr(BRIDGE_URL, "://");
        if (!s) continue;
        s += 3; int bi = 0;
        while (*s && *s != '/' && *s != ':' && bi < 63) bhost[bi++] = *s++;
        if (*s == ':') { s++; bi = 0; while (*s && *s != '/' && bi < 6) bport[bi++] = *s++; bport[bi] = '\0'; }
        char burl[256];
        snprintf(burl, sizeof(burl), "http://%s:%s/api/fund/%s", bhost, bport, s_fund_codes[i]);
        char *bp = http_get(burl);
        if (bp) {
            cJSON *br = cJSON_Parse(bp); free(bp);
            if (br) {
                cJSON *item = cJSON_GetObjectItem(br, "nav");
                if (item) { f->nav = (float)item->valuedouble; got++; }
                cJSON_Delete(br);
            }
        } else {
            ESP_LOGW(TAG, "Fund %s: no data (keeping old nav=%.4f)", f->code, f->nav);
        }
    }
    *count = s_fund_count;
    return (got > 0) ? ESP_OK : ESP_FAIL;
}

/* ===== DeepSeek via bridge ===== */
esp_err_t api_fetch_deepseek(DeepSeekData_t *out)
{
    char *resp = http_get(BRIDGE_URL);
    if (!resp) return ESP_FAIL;
    cJSON *root = cJSON_Parse(resp); free(resp);
    if (!root) return ESP_FAIL;
    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "balance"))) out->balance = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "today_tokens"))) out->today_tokens_m = (float)item->valuedouble / 1e6f;
    if ((item = cJSON_GetObjectItem(root, "today_cost"))) out->today_cost = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "month_tokens"))) out->month_tokens_m = (float)item->valuedouble / 1e6f;
    if ((item = cJSON_GetObjectItem(root, "month_cost"))) out->month_cost = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "cache_hit_rate"))) out->cache_hit_rate = (float)item->valuedouble;
    cJSON_Delete(root);
    if (out->balance > 0) ESP_LOGI(TAG, "DS: %.2f", out->balance);
    return ESP_OK;
}

/* ===== Summary ===== */
void api_calc_summary(AppData_t *app)
{
    float total = 0;
    for (int i = 0; i < app->fund_count; i++) total += app->funds[i].nav * 10000;
    app->total_value = total;
}