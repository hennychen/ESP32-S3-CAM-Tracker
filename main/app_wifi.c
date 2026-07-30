#include "app_wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "app_wifi";

#define NVS_NS      "wifi_cfg"
#define NVS_K_SSID  "ssid"
#define NVS_K_PASS  "pass"

#define AP_SSID_PREFIX  "ESP32-CAM-"
#define AP_PASSWORD     "12345678"    // 至少 8 位
#define AP_CHANNEL      6
#define AP_MAX_CONN     3
#define STA_CONNECT_TIMEOUT_MS  15000
#define STA_MAX_RETRY   6

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_evt;
static int s_retry = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static app_wifi_mode_t s_mode = APP_WIFI_MODE_AP;

/* ---------- NVS 凭据 ---------- */
esp_err_t app_wifi_cfg_load(wifi_creds_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK; // 无
    size_t sl = sizeof(out->ssid), pl = sizeof(out->pass);
    esp_err_t e1 = nvs_get_str(h, NVS_K_SSID, out->ssid, &sl);
    esp_err_t e2 = nvs_get_str(h, NVS_K_PASS, out->pass, &pl);
    nvs_close(h);
    out->valid = (e1 == ESP_OK && e2 == ESP_OK && strlen(out->ssid) > 0);
    return ESP_OK;
}

esp_err_t app_wifi_cfg_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_K_SSID, ssid ? ssid : ""));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_K_PASS, pass ? pass : ""));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved wifi cred: %s", ssid);
    return err;
}

esp_err_t app_wifi_cfg_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_OK;
    nvs_erase_key(h, NVS_K_SSID);
    nvs_erase_key(h, NVS_K_PASS);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------- 事件处理 ---------- */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // 仅当处于 STA 模式（有凭据尝试连接）时才自动连接。
        // APSTA 配网模式下 STA 只用于扫描，不应自动连接。
        if (s_mode == APP_WIFI_MODE_STA) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_mode != APP_WIFI_MODE_STA) return; // 配网模式下不重试
        if (s_retry < STA_MAX_RETRY) {
            esp_wifi_connect(); s_retry++;
            ESP_LOGW(TAG, "sta retry (%d)", s_retry);
        } else {
            xEventGroupSetBits(s_evt, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "sta got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t*)data;
        ESP_LOGI(TAG, "ap client join, aid=%d mac=" MACSTR, ev->aid, MAC2STR(ev->mac));
    }
}

/* ---------- 通用初始化 ---------- */
static void ensure_common(void)
{
    static bool inited = false;
    if (inited) return;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_evt = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,   ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    inited = true;
}

static bool start_sta(const wifi_creds_t *c)
{
    // netif 已在 start_ap_and_sta 中创建
    wifi_config_t wc = { 0 };
    strncpy((char*)wc.sta.ssid,     c->ssid, sizeof(wc.sta.ssid));
    strncpy((char*)wc.sta.password, c->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_evt,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/**
 * 启动 AP + STA 共存：
 *   AP 始终广播（供用户随时接入配网）
 *   STA 若已有凭据则尝试连接（拿到局域网 IP 后可用于视频流）
 */
static void start_ap_and_sta(void)
{
    if (!s_ap_netif)  s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap_wc = { 0 };
    snprintf((char*)ap_wc.ap.ssid, sizeof(ap_wc.ap.ssid),
             AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    ap_wc.ap.ssid_len = strlen((char*)ap_wc.ap.ssid);
    strncpy((char*)ap_wc.ap.password, AP_PASSWORD, sizeof(ap_wc.ap.password));
    ap_wc.ap.channel        = AP_CHANNEL;
    ap_wc.ap.max_connection = AP_MAX_CONN;
    ap_wc.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    if (strlen(AP_PASSWORD) == 0) ap_wc.ap.authmode = WIFI_AUTH_OPEN;

    // APSTA 模式：AP 始终在，用户随时可接入配网
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, " AP always-on (APSTA)");
    ESP_LOGI(TAG, " AP SSID   : %s", ap_wc.ap.ssid);
    ESP_LOGI(TAG, " AP PASS   : %s", AP_PASSWORD);
    ESP_LOGI(TAG, " AP Config : http://192.168.4.1/wifi");
    ESP_LOGI(TAG, "======================================");
}

app_wifi_mode_t app_wifi_start(void)
{
    ensure_common();

    // 一律先启动 APSTA（AP 始终可用）
    s_mode = APP_WIFI_MODE_AP;   // 默认视为 AP，直到 STA 拿到 IP 才升级
    start_ap_and_sta();

    // 若已有凭据，尝试 STA 连接（不影响 AP 广播）
    wifi_creds_t c; app_wifi_cfg_load(&c);
    if (c.valid) {
        ESP_LOGI(TAG, "try STA connect: %s", c.ssid);
        s_mode = APP_WIFI_MODE_STA;  // 先声明进入 STA 尝试，允许事件处理器 auto-reconnect
        if (start_sta(&c)) {
            ESP_LOGI(TAG, "STA connected, AP still available for reconfig");
        } else {
            ESP_LOGW(TAG, "STA connect failed, AP-only usable");
            s_mode = APP_WIFI_MODE_AP;
        }
    } else {
        ESP_LOGI(TAG, "no wifi cred saved, AP-only for provisioning");
    }
    return s_mode;
}

void app_wifi_apply_and_reboot(const char *ssid, const char *pass)
{
    app_wifi_cfg_save(ssid, pass);
    ESP_LOGW(TAG, "restart in 1s to apply new wifi ...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void app_wifi_print_ip(void)
{
    esp_netif_ip_info_t ip;
    if (s_mode == APP_WIFI_MODE_STA && s_sta_netif &&
        esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "STA -> http://" IPSTR "/", IP2STR(&ip.ip));
    } else if (s_ap_netif &&
        esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "AP  -> http://" IPSTR "/", IP2STR(&ip.ip));
    }
}
