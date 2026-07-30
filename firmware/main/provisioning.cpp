/**
 * @file provisioning.cpp
 * @brief WiFi AP 配网 (开放热点 + Captive Portal + 原始 socket HTTP 门户) — 从 main.cpp 拆出
 *
 * 不依赖 esp_http_server: DNS 劫持把所有域名指向 192.168.4.1, HTTP 门户用裸 socket
 * 处理扫描/保存. 用户填 WiFi 凭证存入 NVS(cfg 命名空间)后重启. 5 分钟超时也重启.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <nvs.h>
#include "provisioning.h"
#include "wifi_app.h"

static const char *TAG = "PROV";

/* DNS 服务器 (Captive Portal: 所有域名指向 192.168.4.1) */
static void dns_task(void *pv)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = { .s_addr = htonl(INADDR_ANY) } };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return; }
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from; socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n > 12 && (buf[2] & 0x80) == 0) {
            buf[2] = 0x81; buf[3] = 0x80; buf[7] = 1;
            int q = 12; while (q < n && buf[q] != 0) q++;
            q += 5;
            if (q + 16 <= (int)sizeof(buf)) {
                buf[q] = 0xC0; buf[q+1] = 0x0C;
                buf[q+2] = 0; buf[q+3] = 1;  buf[q+4] = 0; buf[q+5] = 1;
                buf[q+6] = 0; buf[q+7] = 0; buf[q+8] = 0; buf[q+9] = 60;
                buf[q+10] = 0; buf[q+11] = 4;
                buf[q+12] = 192; buf[q+13] = 168; buf[q+14] = 4; buf[q+15] = 1;
                sendto(sock, buf, q + 16, 0, (struct sockaddr *)&from, flen);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* URL 解码 */
static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            int v = 0;
            for (int i = 1; i <= 2; i++) {
                char c = s[i];
                v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
            }
            *d++ = v; s += 3;
        } else { *d++ = *s++; }
    }
    *d = '\0';
}

/* 发送完整 HTTP 响应 (header 用 strlen 计算, 不写死长度) */
static void http_send(int c, const char *status, const char *ctype, const char *body)
{
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
        status, ctype);
    write(c, hdr, hlen);
    if (body) write(c, body, strlen(body));
}

/* 302 跳转到配置门户 (用于 captive portal 探测) */
static void http_redirect_portal(int c)
{
    const char *hdr =
        "HTTP/1.0 302 Found\r\n"
        "Location: http://192.168.4.1/\r\n"
        "Connection: close\r\n\r\n";
    write(c, hdr, strlen(hdr));
}

/* 提取 POST body 里某个表单字段 (application/x-www-form-urlencoded) */
static bool form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n' && i < out_sz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    url_decode(out);   /* 解码 %XX 和 + */
    return true;
}

/* URL 编码 (percent-encoding): 让 SSID 里的空格/特殊字符原样往返, 不被 '_' 替换污染 */
static void url_encode(char *dst, size_t dstsz, const char *src)
{
    static const char *hex = "0123456789ABCDEF";
    size_t d = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s && d + 3 < dstsz; s++) {
        unsigned char c = *s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = (char)c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[(c >> 4) & 0xF];
            dst[d++] = hex[c & 0xF];
        }
    }
    dst[d] = '\0';
}

/* HTML 转义: 放进属性/文本时防止 ' " < > & 破坏页面 (显示名 + value 预填都用) */
static void html_escape(char *dst, size_t dstsz, const char *src)
{
    size_t d = 0;
    for (const char *s = src; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '\'':rep = "&#39;";  break;
            case '"': rep = "&quot;"; break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (d + rl >= dstsz) break;
            memcpy(dst + d, rep, rl); d += rl;
        } else {
            if (d + 1 >= dstsz) break;
            dst[d++] = *s;
        }
    }
    dst[d] = '\0';
}

/* 简易 HTTP 服务器 (原始 socket, 不依赖 esp_http_server) */
static void http_task(void *pv)
{
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) return;
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(80), .sin_addr = { .s_addr = htonl(INADDR_ANY) } };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(srv); return; }
    listen(srv, 3);
    while (1) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int c = accept(srv, (struct sockaddr *)&cli, &clen);
        if (c < 0) continue;
        /* 每个连接设读超时, 防止半开连接阻塞 */
        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* static: http 任务单线程串行处理, 避免大数组占栈导致溢出 */
        static char buf[1536];
        memset(buf, 0, sizeof(buf));
        /* 健壮读取: header 与 body 常分片到达 (甚至 Expect:100-continue),
         * 只读一次会丢掉 POST 表单 body → 配置存不进去. 先读到完整 header, 再按
         * Content-Length 补齐 body. */
        int total = 0;
        while (total < (int)sizeof(buf) - 1) {
            int r = read(c, buf + total, sizeof(buf) - 1 - total);
            if (r <= 0) break;
            total += r;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;   /* header 读完 */
        }
        if (total > 0) {
            /* 客户端若声明 Expect: 100-continue, 先回 100 它才会发送 body */
            if (strstr(buf, "100-continue") || strstr(buf, "100-Continue")) {
                const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
                write(c, cont, strlen(cont));
            }
            /* POST: 按 Content-Length 把 body 收齐 (表单字段就在 body 里) */
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                const char *cl = strstr(buf, "Content-Length:");
                if (!cl) cl = strstr(buf, "content-length:");
                int content_len = cl ? atoi(cl + 15) : 0;
                int body_have = total - (int)((hdr_end + 4) - buf);
                while (body_have < content_len && total < (int)sizeof(buf) - 1) {
                    int r = read(c, buf + total, sizeof(buf) - 1 - total);
                    if (r <= 0) break;
                    total += r;
                    buf[total] = '\0';
                    body_have = total - (int)((hdr_end + 4) - buf);
                }
            }

            /* Captive Portal 探测: 手机连上开放热点后会请求这些 URL 判断是否有网,
             * 统一 302 跳到门户, 手机才会自动弹出配置页并保持连接 */
            if (strstr(buf, "generate_204") || strstr(buf, "gen_204") ||
                strstr(buf, "hotspot-detect") || strstr(buf, "connectivitycheck") ||
                strstr(buf, "ncsi.txt") || strstr(buf, "connecttest")) {
                http_redirect_portal(c);
                close(c);
                continue;
            }

            if (strstr(buf, "GET /scan")) {
                /* 扫描 WiFi (需 APSTA 模式, 纯 AP 下扫描会失败) */
                uint16_t cnt = 16;
                static wifi_ap_record_t rec[16];
                esp_err_t se = esp_wifi_scan_start(NULL, true);
                static char body[4096]; int pos = 0;
                pos += snprintf(body + pos, sizeof(body) - pos,
                    "<html><head><meta charset=utf-8></head><body><h2>选择 WiFi</h2>");
                if (se == ESP_OK && esp_wifi_scan_get_ap_records(&cnt, rec) == ESP_OK) {
                    if (cnt > 16) cnt = 16;
                    for (int i = 0; i < cnt; i++) {
                        if (rec[i].ssid[0] == '\0') continue;
                        /* href 用 URL 编码 (原样往返), 显示用 HTML 转义 (防破坏页面) */
                        char enc[128], disp[208];
                        url_encode(enc, sizeof(enc), (const char *)rec[i].ssid);
                        html_escape(disp, sizeof(disp), (const char *)rec[i].ssid);
                        pos += snprintf(body + pos, sizeof(body) - pos,
                            "<a href='/?s=%s' style='display:block;padding:8px;border:1px solid #ddd;text-decoration:none;color:#333'>%s</a>",
                            enc, disp);
                    }
                } else {
                    pos += snprintf(body + pos, sizeof(body) - pos,
                        "<p>扫描失败, 请手动输入 WiFi 名称</p>");
                }
                snprintf(body + pos, sizeof(body) - pos, "<br><a href='/'>返回</a></body></html>");
                http_send(c, "200 OK", "text/html", body);
            } else if (strstr(buf, "POST /save")) {
                /* 保存配置: 只覆盖 cfg 命名空间的 ssid/pass, 不擦除其它 NVS 数据 */
                char ssid[33] = "", pass[65] = "";
                bool has_ssid = form_get(buf, "ssid", ssid, sizeof(ssid));
                form_get(buf, "pass", pass, sizeof(pass));
                bool saved = false;
                if (has_ssid && ssid[0]) {
                    nvs_handle_t nv;
                    if (nvs_open("cfg", NVS_READWRITE, &nv) == ESP_OK) {
                        /* 先把当前主网络降为备用槽 (不同网络才备份, 避免重复写) */
                        char cur[33] = {};
                        size_t csz = sizeof(cur);
                        if (nvs_get_str(nv, "ssid", cur, &csz) == ESP_OK &&
                            cur[0] && strcmp(cur, ssid) != 0) {
                            char curp[65] = {};
                            csz = sizeof(curp);
                            nvs_get_str(nv, "pass", curp, &csz);
                            nvs_set_str(nv, "ssid2", cur);
                            nvs_set_str(nv, "pass2", curp);
                            ESP_LOGI(TAG, "Backup prev WiFi: %s", cur);
                        }
                        esp_err_t e1 = nvs_set_str(nv, "ssid", ssid);
                        esp_err_t e2 = nvs_set_str(nv, "pass", pass);
                        esp_err_t ce = nvs_commit(nv);
                        nvs_close(nv);
                        /* 三者都成功才算存好; set 失败(如分区满)时 commit 仍返回 0 */
                        saved = (e1 == ESP_OK && e2 == ESP_OK && ce == ESP_OK);
                        ESP_LOGI(TAG, "Saved WiFi cfg: ssid='%s' pass_len=%d set=%d/%d commit=%d",
                                 ssid, (int)strlen(pass), e1, e2, ce);
                    } else {
                        ESP_LOGE(TAG, "POST /save: nvs_open cfg fail");
                    }
                } else {
                    ESP_LOGW(TAG, "POST /save: ssid 解析失败 (body 未收全?), total=%d", total);
                }
                if (saved) {
                    /* 只有真正存成功才重启, 避免"假成功"重启进坏状态 */
                    http_send(c, "200 OK", "text/html",
                        "<html><head><meta charset=utf-8></head><body>"
                        "<h2>已保存, 正在重启...</h2></body></html>");
                    close(c); close(srv);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    esp_restart();
                    return;
                }
                /* 保存失败: 回错误页让用户重试, 不重启 */
                http_send(c, "200 OK", "text/html",
                    "<html><head><meta charset=utf-8></head><body>"
                    "<h2>保存失败, 请返回重试</h2><a href='/'>返回</a></body></html>");
            } else {
                /* 首页 (含根路径和被劫持的其它域名) */
                char sel[64] = "";
                const char *q = strstr(buf, "?s=");
                if (q) { q += 3; int i = 0; while (*q && *q != ' ' && *q != '&' && i < 63) sel[i++] = *q++; sel[i] = '\0'; url_decode(sel); }
                char sel_esc[208];
                html_escape(sel_esc, sizeof(sel_esc), sel);   /* 预填进 value='' 前转义, 防 ' 破坏属性 */
                static char body[4096];
                snprintf(body, sizeof(body),
                    "<html><head><meta charset=utf-8><meta name=viewport content='width=320,initial-scale=1'>"
                    "<style>body{font:14px sans-serif;margin:16px;text-align:center}"
                    "input,button{width:90%%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box}"
                    "button{background:#2d7;color:#fff;border:none;font-size:16px;cursor:pointer}</style>"
                    "</head><body><h2>RLCD 配置</h2>"
                    "<a href='/scan' style='display:inline-block;width:90%%;padding:8px;margin:4px;background:#2d7;color:#fff;border-radius:4px;text-decoration:none;font-size:16px;box-sizing:border-box'>扫描 WiFi</a>"
                    "<form action=/save method=post>"
                    "<input name=ssid placeholder='WiFi 名称' value='%s'><br>"
                    "<input type=password name=pass placeholder='WiFi 密码'><br>"
                    "<button>保存并重启</button>"
                    "</form></body></html>", sel_esc);
                http_send(c, "200 OK", "text/html", body);
            }
        }
        close(c);
    }
    close(srv);
}

void start_ap_provision(void)
{
    ESP_LOGI(TAG, "Starting AP provision...");
    /* 先停掉 STA 自动重连, 否则重连与 WiFi 扫描抢射频, 导致扫描卡死/时好时坏 */
    wifi_stop_sta_reconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) { ESP_LOGE(TAG, "AP netif fail"); return; }
    /* APSTA: STA 接口保留才能扫描 WiFi (纯 AP 模式无法 scan) */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t wc = {};
    memcpy(wc.ap.ssid, "RLCD-AP", 7);
    wc.ap.ssid_len = 7;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_wifi_start();
    ESP_LOGI(TAG, "AP 'RLCD-AP' started");

    /* DNS 劫持 + HTTP 服务器 */
    xTaskCreate(dns_task, "dns", 4096, NULL, 3, NULL);
    xTaskCreate(http_task, "http", 12288, NULL, 3, NULL);
    ESP_LOGI(TAG, "Services started");

    /* 等待 5 分钟 */
    for (int i = 0; i < 300; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Timeout, restarting...");
    esp_restart();
}
