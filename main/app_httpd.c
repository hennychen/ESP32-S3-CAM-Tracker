#include "app_httpd.h"
#include "app_camera.h"
#include "app_face_detect.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "app_httpd";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE  = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY      = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART          = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %lld\r\n\r\n";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t wifi_html_start[]  asm("_binary_wifi_html_start");
extern const uint8_t wifi_html_end[]    asm("_binary_wifi_html_end");

static face_result_t   s_face = {0};
static SemaphoreHandle_t s_face_lock;

void app_httpd_set_face(const face_result_t *r)
{
    xSemaphoreTake(s_face_lock, portMAX_DELAY);
    s_face = *r;
    xSemaphoreGive(s_face_lock);
}

/* ============ 页面 ============ */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start);
}

static esp_err_t wifi_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char*)wifi_html_start, wifi_html_end - wifi_html_start);
}

/* ============ 视频 & 检测 ============ */
static esp_err_t face_handler(httpd_req_t *req)
{
    char buf[384];
    xSemaphoreTake(s_face_lock, portMAX_DELAY);
    face_result_t f = s_face;
    xSemaphoreGive(s_face_lock);

    int n = snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"score\":%.2f,\"frame\":%lu,\"iw\":%d,\"ih\":%d,"
        "\"kp\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"roll\":%.1f,\"vr\":%.2f,\"drowsy\":%s}",
        f.valid ? "true" : "false", f.x, f.y, f.w, f.h,
        f.score, (unsigned long)f.frame_id, f.img_w, f.img_h,
        f.kp[0], f.kp[1], f.kp[2], f.kp[3], f.kp[4],
        f.kp[5], f.kp[6], f.kp[7], f.kp[8], f.kp[9],
        f.roll, f.vert_ratio, f.drowsy ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 把最新人脸结果附加到 HTTP 头，前端一次请求即可拿到帧+人脸
    face_result_t f;
    xSemaphoreTake(s_face_lock, portMAX_DELAY);
    f = s_face;
    xSemaphoreGive(s_face_lock);

    char face_hdr[128];
    snprintf(face_hdr, sizeof(face_hdr), "%d,%d,%d,%d,%d,%.2f,%lu,%d,%d",
             f.valid ? 1 : 0, f.x, f.y, f.w, f.h, f.score,
             (unsigned long)f.frame_id, f.img_w, f.img_h);
    httpd_resp_set_hdr(req, "X-Face", face_hdr);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=cap.jpg");
    esp_err_t r = httpd_resp_send(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return r;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part_buf[80];
    int64_t last = 0;
    uint32_t frames = 0;
    while (1) {
        // 直接推送原始 JPEG，人脸框由前端叠加显示（避免解码+重编码导致流卡顿）
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        face_result_t f;
        xSemaphoreTake(s_face_lock, portMAX_DELAY);
        f = s_face;
        xSemaphoreGive(s_face_lock);

        int64_t ts = esp_timer_get_time();
        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                               (unsigned)fb->len, ts);

        if ((res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY))) != ESP_OK ||
            (res = httpd_resp_send_chunk(req, part_buf, hlen)) != ESP_OK ||
            (res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len)) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        esp_camera_fb_return(fb);

        // 每 30 帧打印一次实际 FPS
        frames++;
        if (frames % 30 == 0) {
            if (last) {
                float fps = 30.0f * 1000000.0f / (float)(ts - last);
                ESP_LOGI(TAG, "stream fps=%.1f face=%s", fps, f.valid ? "Y" : "N");
            }
            last = ts;
        }
    }

    ESP_LOGI(TAG, "stream ended");
    return res;
}

/* ============ WiFi 配置 API ============ */

/** URL-encoded body -> key/value 抽取 */
static bool url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dst_size; i++) {
        char c = src[i];
        if (c == '+') dst[di++] = ' ';
        else if (c == '%' && src[i+1] && src[i+2]) {
            char hx[3] = { src[i+1], src[i+2], 0 };
            dst[di++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else dst[di++] = c;
    }
    dst[di] = 0;
    return true;
}

static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *amp = strchr(eq, '&');
        size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
        if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            char tmp[128] = {0};
            size_t cp = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
            memcpy(tmp, eq + 1, cp);
            url_decode(tmp, out, outsz);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

/** GET /wifi/status  -> 当前 SSID / 模式 / IP */
static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    wifi_creds_t c; app_wifi_cfg_load(&c);
    wifi_mode_t m; esp_wifi_get_mode(&m);
    const char *mode = (m == WIFI_MODE_STA)   ? "STA" :
                       (m == WIFI_MODE_AP)    ? "AP"  :
                       (m == WIFI_MODE_APSTA) ? "APSTA" : "NONE";

    // STA IP：若已连接返回具体 IP，否则空串
    char sta_ip[16] = "";
    bool sta_connected = false;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip.ip));
            sta_connected = true;
        }
    }

    // 本机 MAC（用于路由器查找）
    uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // mDNS 主机名
    char host[32];
    snprintf(host, sizeof(host), "esp32-cam-%02x%02x", mac[4], mac[5]);

    char buf[384];
    int n = snprintf(buf, sizeof(buf),
        "{\"mode\":\"%s\",\"saved_ssid\":\"%s\",\"configured\":%s,"
        "\"sta_connected\":%s,\"sta_ip\":\"%s\","
        "\"ap_ip\":\"192.168.4.1\",\"mac\":\"%s\","
        "\"hostname\":\"%s\"}",
        mode,
        c.valid ? c.ssid : "",
        c.valid ? "true" : "false",
        sta_connected ? "true" : "false",
        sta_ip, mac_str, host);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, n);
}

/** GET /wifi/scan  -> 附近 AP 列表 */
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "/wifi/scan begin");

    // 1. 扫描 WiFi
    //    默认每信道 active 扫描 120ms（全 13 信道 ~1.5s），避免超过 httpd 默认 5s 超时
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 120,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);  // true = 阻塞
    if (err != ESP_OK) {
        // 返回合法 JSON 便于前端解析，同时打印错误码
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s (0x%x)",
                 esp_err_to_name(err), err);
        char emsg[128];
        int en = snprintf(emsg, sizeof(emsg),
            "{\"error\":\"scan_failed\",\"code\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_send(req, emsg, en);
        return ESP_FAIL;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }
    if (num > 25) num = 25; // 限制数量防止内存溢出

    wifi_ap_record_t *aps = calloc(num, sizeof(wifi_ap_record_t));
    if (!aps) { httpd_resp_send_500(req); return ESP_FAIL; }
    esp_wifi_scan_get_ap_records(&num, aps);

    // 2. 在内存中构建完整的 JSON 字符串
    // 估算所需 buffer 大小: "[]" + num * ("{\"ssid\":\"...\",\"rssi\":-100,\"auth\":4}," -> ~100 字节/项)
    size_t buf_size = 2 + num * 100;
    char *buf = malloc(buf_size);
    if (!buf) { free(aps); httpd_resp_send_500(req); return ESP_FAIL; }

    char *p = buf;
    p += snprintf(p, buf_size - (p - buf), "[");
    for (uint16_t i = 0; i < num; ++i) {
        // JSON 字符串转义 (简易版：只处理引号和反斜杠)
        char ssid_escaped[66] = {0};
        const char *s_in = (const char *)aps[i].ssid;
        char *s_out = ssid_escaped;
        while (*s_in && s_out < ssid_escaped + sizeof(ssid_escaped) - 2) {
            if (*s_in == '\\' || *s_in == '"') {
                *s_out++ = '\\';
            }
            *s_out++ = *s_in++;
        }

        p += snprintf(p, buf_size - (p - buf),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
            (i == 0) ? "" : ",", ssid_escaped, aps[i].rssi, aps[i].authmode);
        if (p - buf >= (ptrdiff_t)buf_size - 100) { // 检查溢出
            ESP_LOGW(TAG, "wifi scan json truncated");
            break;
        }
    }
    p += snprintf(p, buf_size - (p - buf), "]");

    // 3. 一次性发送
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, p - buf);

    // 4. 清理
    free(buf);
    free(aps);
    return ESP_OK;
}

/** 延迟重启任务：让 HTTP 响应先发送完毕，再让固件重启 */
static void delayed_restart_task(void *arg)
{
    int ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    ESP_LOGW(TAG, "restart now to apply new wifi ...");
    esp_restart();
}

/** POST /wifi/save  body: ssid=xxx&pass=yyy */
static esp_err_t wifi_save_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) break;
        total += r;
    }

    char ssid[64] = {0}, pass[128] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_ssid\"}");
        return ESP_FAIL;
    }
    form_get(body, "pass", pass, sizeof(pass));

    // 保存凭据（先落盘）
    app_wifi_cfg_save(ssid, pass);

    // 读取本机 MAC（用于提示用户在路由器中查找该 MAC 对应的 IP）
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 返回 JSON 给前端（前端会据此展示提示页），转义 ssid 里的引号/反斜杠
    char ssid_esc[128] = {0};
    const char *sp = ssid; char *dp = ssid_esc;
    while (*sp && dp < ssid_esc + sizeof(ssid_esc) - 2) {
        if (*sp == '\\' || *sp == '"') *dp++ = '\\';
        *dp++ = *sp++;
    }

    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"ssid\":\"%s\",\"mac\":\"%s\",\"reboot_in_ms\":2000}",
        ssid_esc, mac_str);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, resp, n);

    // 起一个任务延迟 2s 重启，让 TCP 响应先落到手机
    xTaskCreate(delayed_restart_task, "reboot", 2048,
                (void *)(intptr_t)2000, 5, NULL);
    return ESP_OK;
}

/** POST /wifi/reset  清除凭据后重启 -> 进入 AP */
static esp_err_t wifi_reset_handler(httpd_req_t *req)
{
    app_wifi_cfg_clear();
    httpd_resp_sendstr(req, "cleared, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ============ Captive Portal ============
 * 手机在连接 AP 后会自动请求"探测 URL"来判断是否需要登录，如：
 *   iOS      /hotspot-detect.html, /library/test/success.html
 *   Android  /generate_204, /gen_204
 *   Windows  /ncsi.txt, /connecttest.txt
 * 只要这些请求得到 302 -> /wifi 的响应，系统就会弹出配网页面。
 * 配合 DNS 劫持（所有查询都返回 192.168.4.1）即可覆盖各种系统。
 */
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    // 仅对 AP 侧客户端（192.168.4.x）做 captive 重定向
    // STA 侧客户端返回标准 404，避免把 STA 网络的用户重定向到不可达的 192.168.4.1
    int sockfd = httpd_req_to_sockfd(req);
    bool from_ap = false;
    if (sockfd >= 0) {
        struct sockaddr_in6 addr6;
        socklen_t addr_size = sizeof(addr6);
        if (getpeername(sockfd, (struct sockaddr *)&addr6, &addr_size) == 0) {
            // 兼容 IPv4-mapped IPv6：取最后 4 字节比对 192.168.4.x
            const uint8_t *b = (const uint8_t *)&addr6.sin6_addr;
            // IPv4-mapped: ::ffff:a.b.c.d，前 10 字节 0，第 11-12 字节 0xff
            uint8_t ip[4];
            if (addr6.sin6_family == AF_INET6 &&
                b[10] == 0xff && b[11] == 0xff) {
                memcpy(ip, b + 12, 4);
            } else {
                // 纯 IPv4
                struct sockaddr_in *v4 = (struct sockaddr_in *)&addr6;
                memcpy(ip, &v4->sin_addr, 4);
            }
            from_ap = (ip[0] == 192 && ip[1] == 168 && ip[2] == 4);
        }
    }

    if (!from_ap) {
        // STA 侧：返回标准 404 但导向根路径（对 /favicon.ico 等友好）
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req,
            "<html><body><h3>404 Not Found</h3>"
            "<p><a href=\"/\">Go to camera home</a></p></body></html>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // AP 侧：captive portal 重定向到配网页
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<html><body>Redirecting to <a href=\"http://192.168.4.1/wifi\">config</a>...</body></html>",
        HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- 极简 DNS 劫持：所有 A 查询都回 192.168.4.1 ---- */
#define DNS_PORT           53
#define DNS_MAX_LEN        512
#define AP_GATEWAY_BE_IP   0x0104A8C0  // 192.168.4.1 网络字节序

static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "dns socket fail"); vTaskDelete(NULL); return; }

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(DNS_PORT);
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "dns bind fail: errno=%d", errno);
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "captive DNS hijack running on :53 -> 192.168.4.1");

    uint8_t buf[DNS_MAX_LEN];
    struct sockaddr_in from; socklen_t flen = sizeof(from);
    while (1) {
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        // 构造响应：把 flags 改成 QR=1,RA=1（0x8180），保留 QDCOUNT=1，
        // ANCOUNT 置 1；把 question 之后追加一条 A 记录指向 192.168.4.1
        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;   // ANCOUNT = 1
        buf[8] = 0x00; buf[9] = 0x00;   // NSCOUNT
        buf[10]= 0x00; buf[11]= 0x00;   // ARCOUNT

        // 定位 question 末尾（跳过 QNAME 后 4 字节 QTYPE+QCLASS）
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
            if (p >= n) break;
        }
        p += 5; // \0 + QTYPE(2) + QCLASS(2)
        if (p + 16 > (int)sizeof(buf)) continue;

        // Answer: name pointer to 0x0c, TYPE=A, CLASS=IN, TTL=60, RDLEN=4, RDATA
        buf[p++] = 0xC0; buf[p++] = 0x0C;
        buf[p++] = 0x00; buf[p++] = 0x01;
        buf[p++] = 0x00; buf[p++] = 0x01;
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C;
        buf[p++] = 0x00; buf[p++] = 0x04;
        uint32_t ip_be = AP_GATEWAY_BE_IP;
        memcpy(buf + p, &ip_be, 4); p += 4;

        sendto(sock, buf, p, 0, (struct sockaddr *)&from, flen);
    }
}

void app_httpd_start_captive_dns(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreate(dns_hijack_task, "dns_hijack", 3072, NULL, 4, NULL);
}

/* ============ 启动 ============ */
esp_err_t app_httpd_start(void)
{
    s_face_lock = xSemaphoreCreateMutex();

    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size  = 8192;
    cfg.max_uri_handlers = 16;
    cfg.core_id = 0;
    cfg.lru_purge_enable = true;
    // WiFi 扫描/写 NVS 等可能耗时 >5s，延长收发超时避免手机侧收到 500
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t u_index    = { .uri="/",             .method=HTTP_GET,  .handler=index_handler };
    httpd_uri_t u_wifi     = { .uri="/wifi",         .method=HTTP_GET,  .handler=wifi_page_handler };
    httpd_uri_t u_face     = { .uri="/face",         .method=HTTP_GET,  .handler=face_handler };
    httpd_uri_t u_cap      = { .uri="/capture",      .method=HTTP_GET,  .handler=capture_handler };
    httpd_uri_t u_stream   = { .uri="/stream",       .method=HTTP_GET,  .handler=stream_handler };
    httpd_uri_t u_wstatus  = { .uri="/wifi/status",  .method=HTTP_GET,  .handler=wifi_status_handler };
    httpd_uri_t u_wscan    = { .uri="/wifi/scan",    .method=HTTP_GET,  .handler=wifi_scan_handler };
    httpd_uri_t u_wsave    = { .uri="/wifi/save",    .method=HTTP_POST, .handler=wifi_save_handler };
    httpd_uri_t u_wreset   = { .uri="/wifi/reset",   .method=HTTP_POST, .handler=wifi_reset_handler };

    httpd_register_uri_handler(server, &u_index);
    httpd_register_uri_handler(server, &u_wifi);
    httpd_register_uri_handler(server, &u_face);
    httpd_register_uri_handler(server, &u_cap);
    httpd_register_uri_handler(server, &u_stream);
    httpd_register_uri_handler(server, &u_wstatus);
    httpd_register_uri_handler(server, &u_wscan);
    httpd_register_uri_handler(server, &u_wsave);
    httpd_register_uri_handler(server, &u_wreset);

    // Captive Portal: 未匹配的路径一律 302 -> /wifi
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_404_handler);

    ESP_LOGI(TAG, "HTTP server started on :80");
    return ESP_OK;
}
