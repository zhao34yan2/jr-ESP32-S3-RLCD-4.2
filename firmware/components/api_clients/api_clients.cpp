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
#include <nvs_flash.h>
#include <nvs.h>
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
                /* 调试: 打印原始字节 */
                ESP_LOGI(TAG, "FC[%d]: code=%d raw=%02x%02x%02x%02x%02x%02x",
                         i, wc,
                         (uint8_t)out->fc_cond[i][0],
                         (uint8_t)out->fc_cond[i][1],
                         (uint8_t)out->fc_cond[i][2],
                         (uint8_t)out->fc_cond[i][3],
                         (uint8_t)out->fc_cond[i][4],
                         (uint8_t)out->fc_cond[i][5]);
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

/* ===== HTTPS GET using esp_http_client (支持自定义头) ===== */
static char *https_get(const char *url, const char *header_key, const char *header_val)
{
    esp_http_client_config_t cfg = {0};
    cfg.url = url; cfg.timeout_ms = 5000; cfg.buffer_size = 4096;
    cfg.skip_cert_common_name_check = true;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    if (header_key && header_val) {
        esp_http_client_set_header(client, header_key, header_val);
    }
    char *buf = (char *)malloc(4096);
    if (!buf) { esp_http_client_cleanup(client); return NULL; }
    memset(buf, 0, 4096);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int len = esp_http_client_read(client, buf, 4095);
        if (len > 0) buf[len] = '\0';
    } else {
        ESP_LOGW(TAG, "HTTPS fail: %s (%s)", url, esp_err_to_name(err));
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }
    esp_http_client_cleanup(client);
    return buf;
}

static void get_bridge_base(char *buf, size_t sz);

/* ===== Funds (fundgz + East Money HTTPS fallback) ===== */
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

        /* Fallback to Bridge */
        char base[64];
        get_bridge_base(base, sizeof(base));
        char burl[256];
        snprintf(burl, sizeof(burl), "%s/api/fund/%s", base, s_fund_codes[i]);
        char *bp = http_get(burl);
        if (bp) {
            ESP_LOGI(TAG, "Bridge resp: %.60s", bp);
            cJSON *br = cJSON_Parse(bp); free(bp);
            if (br) {
                cJSON *item = cJSON_GetObjectItem(br, "nav");
                if (item) { f->nav = (float)item->valuedouble; got++; }
                else { ESP_LOGW(TAG, "Bridge: no nav field"); }
                cJSON_Delete(br);
            } else { ESP_LOGW(TAG, "Bridge: JSON parse fail"); }
        } else { ESP_LOGW(TAG, "Fund %s: Bridge fallback fail", f->code); }
    }
    *count = s_fund_count;
    return (got > 0) ? ESP_OK : ESP_FAIL;
}

/* ===== Bridge URL 管理 (NVS 持久化, 免重新编译) ===== */
static char s_bridge_url[128] = "";

const char *get_bridge_url(void)
{
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
    return s_bridge_url;
}

static void get_bridge_base(char *buf, size_t sz)
{
    const char *url = get_bridge_url();
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
        strlcpy(s_bridge_url, url, sizeof(s_bridge_url));
        ESP_LOGI(TAG, "Bridge URL updated: %s", url);
    }
}

/* ===== DeepSeek 直连 (HTTPS, 不依赖 Bridge) ===== */
esp_err_t api_fetch_deepseek(DeepSeekData_t *out)
{
    char *resp = http_get(get_bridge_url());
    if (!resp) return ESP_FAIL;
    cJSON *root = cJSON_Parse(resp); free(resp);
    if (!root) return ESP_FAIL;

    /* Bridge 格式 (有 today_tokens) */
    cJSON *tt = cJSON_GetObjectItem(root, "today_tokens");
    if (tt) {
        out->today_tokens_m = (float)tt->valuedouble / 1e6f;
        cJSON *i;
        if ((i = cJSON_GetObjectItem(root, "balance")))     out->balance      = (float)i->valuedouble;
        if ((i = cJSON_GetObjectItem(root, "today_cost"))) out->today_cost    = (float)i->valuedouble;
        if ((i = cJSON_GetObjectItem(root, "month_tokens")))out->month_tokens_m=(float)i->valuedouble/1e6f;
        if ((i = cJSON_GetObjectItem(root, "month_cost")))  out->month_cost   = (float)i->valuedouble;
        if ((i = cJSON_GetObjectItem(root, "cache_hit_rate")))out->cache_hit_rate=(float)i->valuedouble;
    } else {
        /* DeepSeek 官方 API 格式 */
        cJSON *infos = cJSON_GetObjectItem(root, "balance_infos");
        if (infos && cJSON_IsArray(infos) && cJSON_GetArraySize(infos) > 0) {
            cJSON *info = cJSON_GetArrayItem(infos, 0);
            cJSON *tb = cJSON_GetObjectItem(info, "total_balance");
            if (tb && tb->valuestring) out->balance = (float)atof(tb->valuestring);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "DS: %.2f", out->balance);
    return ESP_OK;
}

/* ===== Summary ===== */
void api_calc_summary(AppData_t *app)
{
    float total = 0;
    for (int i = 0; i < app->fund_count; i++) total += app->funds[i].nav * 10000;
    app->total_value = total;
}