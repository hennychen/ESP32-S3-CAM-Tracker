// ============================================================================
// ESP32-S3-CAM Face Tracker  (ESP-IDF)
//
// 启动流程：
//   1) 相机初始化 (OV5640, JPEG, PSRAM 双缓冲)
//   2) 云台 LEDC PWM 初始化
//   3) WiFi：
//        - 有保存凭据 -> STA 连接
//        - 无凭据或连接失败 -> 开启 AP (ESP32-CAM-xxxx / 12345678)
//     用户可通过 http://<ip>/wifi 配网
//   4) HTTP 服务器 (视频 / 检测结果 / 配网 API)
//   5) 人脸检测任务（ESP-DL HumanFaceDetect） -> 云台跟随
// ============================================================================
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "mdns.h"

#include "app_camera.h"
#include "app_wifi.h"
#include "app_httpd.h"
#include "app_gimbal.h"
#include "app_face_detect.h"
#include "app_sdcard.h"

static const char *TAG = "main";

/** 启动 mDNS：让用户可用 http://esp32-cam.local/ 访问 */
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[32];
    // 主机名带 MAC 后 4 位便于多台设备共存 (esp32-cam-faa8)
    snprintf(host, sizeof(host), "esp32-cam-%02x%02x", mac[4], mac[5]);
    mdns_hostname_set(host);
    mdns_instance_name_set("ESP32-CAM Face Tracker");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    // 同时注册通用别名 esp32-cam.local （仅第一台，多台会冲突自然回退）
    mdns_hostname_set("esp32-cam");
    ESP_LOGI(TAG, "mDNS started: http://%s.local  and  http://esp32-cam.local", host);
}

// 检测输入分辨率：ESP-DL human_face_detect 通常 240x240 / 320x240 效率最佳
#define DET_FRAMESIZE   FRAMESIZE_HVGA        // 480x320
// 若模型较大导致内存吃紧，可改用 FRAMESIZE_QVGA(320x240) 或 FRAMESIZE_240X240

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3-CAM Face Tracker ===");
    ESP_LOGI(TAG, "internal heap : %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "psram    heap : %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // 0) TF 卡挂载（暂时禁用：GPIO 38/39/40 与 Octal PSRAM 内部信号有潜在冲突，
    //    v0.10 排查 SD 引脚后再开启）
    // esp_err_t sd_err = app_sdcard_mount();
    // if (sd_err != ESP_OK) {
    //     ESP_LOGW(TAG, "TF card not available: %s", esp_err_to_name(sd_err));
    // }

    // 1) WiFi 先启动（STA 优先，失败降级 AP 配网）
    //    这样即便摄像头/云台初始化失败，也能通过 AP 观察和排障
    app_wifi_mode_t mode = app_wifi_start();
    app_wifi_print_ip();
    if (mode == APP_WIFI_MODE_AP) {
        ESP_LOGW(TAG, ">>> 请连接开发板 WiFi，浏览器打开 http://192.168.4.1/wifi 完成配网");
    }

    // 2) 摄像头：JPEG 输出 —— /stream 零转换直接推流；
    //    检测任务里用 jpg2rgb565 解码送 ESP-DL。
    //    失败不 abort，仅打印错误，允许仅 WiFi 配网使用
    esp_err_t cam_err = app_camera_init(DET_FRAMESIZE, PIXFORMAT_JPEG);
    if (cam_err != ESP_OK) {
        ESP_LOGE(TAG, "camera init FAILED: %s. video/detect will be disabled.",
                 esp_err_to_name(cam_err));
    }

    // 3) 云台
    app_gimbal_init(480, 320);

    // 4) HTTP MJPEG + JSON + 配网 API
    app_httpd_start();

    // AP 始终广播，所以 Captive Portal 也一直开启（DNS 劫持 + 404 重定向 /wifi）
    // STA 已连时 DNS 劫持仅影响连到 AP 的客户端，不影响 STA 侧访问
    app_httpd_start_captive_dns();

    // 4.5) mDNS：仅 STA 已连接时才注册（AP-only 时局域网不通）
    if (mode == APP_WIFI_MODE_STA) {
        start_mdns();
    }

    // 5) 人脸检测任务（仅当摄像头正常）
    if (cam_err == ESP_OK) {
        app_face_detect_start();
    }

    ESP_LOGI(TAG, "boot done. open browser -> http://<ip>/");
}
