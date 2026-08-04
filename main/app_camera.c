#include "app_camera.h"
#include "camera_pins.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "app_camera";

esp_err_t app_camera_init(framesize_t frame_size, pixformat_t pixel_format)
{
    camera_config_t cfg = {
        .pin_pwdn       = PWDN_GPIO_NUM,
        .pin_reset      = RESET_GPIO_NUM,
        .pin_xclk       = XCLK_GPIO_NUM,
        .pin_sccb_sda   = SIOD_GPIO_NUM,
        .pin_sccb_scl   = SIOC_GPIO_NUM,
        .pin_d7         = Y9_GPIO_NUM,
        .pin_d6         = Y8_GPIO_NUM,
        .pin_d5         = Y7_GPIO_NUM,
        .pin_d4         = Y6_GPIO_NUM,
        .pin_d3         = Y5_GPIO_NUM,
        .pin_d2         = Y4_GPIO_NUM,
        .pin_d1         = Y3_GPIO_NUM,
        .pin_d0         = Y2_GPIO_NUM,
        .pin_vsync      = VSYNC_GPIO_NUM,
        .pin_href       = HREF_GPIO_NUM,
        .pin_pclk       = PCLK_GPIO_NUM,
        .xclk_freq_hz   = 24000000,           // D: 24MHz sensor 上限，+20% 采集速度
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,
        .pixel_format   = pixel_format,
        .frame_size     = frame_size,
        .jpeg_quality   = 15,                 // E: q=15，payload 减少 ~15%
        .fb_count       = 4,                  // P1: 四缓冲，消除 face_task 与 /capture 并发时的 1500ms 毛刺
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };

    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        ESP_LOGW(TAG, "PSRAM not found, degrade to DRAM single buffer");
        cfg.fb_location = CAMERA_FB_IN_DRAM;
        cfg.fb_count    = 1;
        cfg.frame_size  = FRAMESIZE_QVGA;
        cfg.jpeg_quality = 20;
    }

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "sensor PID=0x%04X (0x5640=OV5640, 0x2640=OV2640)", s->id.PID);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_lenc(s, 1);
        s->set_dcw(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
    }
    return ESP_OK;
}

camera_fb_t *app_camera_get(void)  { return esp_camera_fb_get(); }
void         app_camera_return(camera_fb_t *fb) { if (fb) esp_camera_fb_return(fb); }

esp_err_t app_camera_set_framesize(framesize_t s)
{
    sensor_t *sen = esp_camera_sensor_get();
    if (!sen) return ESP_FAIL;
    return sen->set_framesize(sen, s) == 0 ? ESP_OK : ESP_FAIL;
}
