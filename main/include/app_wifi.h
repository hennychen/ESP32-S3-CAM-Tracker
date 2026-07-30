#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX_LEN  33
#define WIFI_PASS_MAX_LEN  65

/**
 * WiFi 运行模式：
 *   MODE_STA - 使用已保存的 SSID/PWD 连接路由器（正常工作）
 *   MODE_AP  - 首次上电或连接失败 -> 开启 AP 供用户配网
 */
typedef enum {
    APP_WIFI_MODE_STA = 0,
    APP_WIFI_MODE_AP  = 1,
} app_wifi_mode_t;

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    char pass[WIFI_PASS_MAX_LEN];
    bool valid;
} wifi_creds_t;

/** 从 NVS 加载凭据；未配置 -> valid=false */
esp_err_t app_wifi_cfg_load(wifi_creds_t *out);

/** 保存到 NVS */
esp_err_t app_wifi_cfg_save(const char *ssid, const char *pass);

/** 清除保存的凭据（下次上电进入配网 AP） */
esp_err_t app_wifi_cfg_clear(void);

/**
 * 启动 WiFi：
 *   有凭据 -> STA 连接，超时后自动降级为 AP
 *   无凭据 -> 直接 AP
 * 返回当前实际模式。
 */
app_wifi_mode_t app_wifi_start(void);

/** 请求切换：保存新凭据 -> 重启到 STA 模式（供 HTTP 回调调用） */
void app_wifi_apply_and_reboot(const char *ssid, const char *pass);

/** 用于日志打印当前 IP */
void app_wifi_print_ip(void);

#ifdef __cplusplus
}
#endif
