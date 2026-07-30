// ============================================================================
// ESP-DL 人脸检测任务（v0.9：与推流解耦）
// - 独立按自身节奏抓帧检测；推流由 stream_handler 直接调 esp_camera_fb_get()
// - 检测结果通过 app_httpd_set_face() 单向推送坐标
// - 预分配 RGB 缓冲避免每帧 malloc 抖动
// - 自适应检测间隔：连续无脸帧数增加时降低检测频率，把 CPU 让给推流
// ============================================================================
extern "C" {
#include "app_face_detect.h"
#include "app_camera.h"
#include "app_gimbal.h"
#include "app_httpd.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
}

#include <list>
#include "human_face_detect.hpp"
#include "dl_image_define.hpp"
#include "dl_detect_define.hpp"

static const char *TAG = "face_det";

// --- 检测结果 ---
static SemaphoreHandle_t s_res_lock;
static face_result_t     s_last = {};

extern "C" void app_face_detect_get(face_result_t *out)
{
    xSemaphoreTake(s_res_lock, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_res_lock);
}

// 保留 lock/unlock 接口（其他 API 仍在使用），但内部返回空避免误用
extern "C" bool app_face_detect_frame_lock(const uint8_t **buf, size_t *len)
{
    (void)buf; (void)len;
    return false;
}
extern "C" void app_face_detect_frame_unlock(void) {}

static void publish_result(const face_result_t &r)
{
    xSemaphoreTake(s_res_lock, portMAX_DELAY);
    s_last = r;
    xSemaphoreGive(s_res_lock);
    app_httpd_set_face(&r);
}

static void face_task(void *arg)
{
    ESP_LOGI(TAG, "face detect task start (decoupled from stream)");
    HumanFaceDetect *det = new HumanFaceDetect();

    // C: 预分配 RGB 缓冲（按 HVGA 480x320 上限，兼容更大分辨率时可下调）
    const size_t rgb_cap = 640 * 480 * 2;
    uint8_t *rgb = (uint8_t *)heap_caps_malloc(rgb_cap, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "no PSRAM for RGB buffer");
        vTaskDelete(NULL); return;
    }

    uint32_t frame_id = 0;
    uint32_t miss_streak = 0;   // 连续未检出人脸的帧数
    uint32_t det_count  = 0;
    int64_t  det_t0 = esp_timer_get_time();

    while (true) {
        camera_fb_t *fb = app_camera_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        int w = fb->width, h = fb->height;
        size_t need = (size_t)w * h * 2;
        bool decoded = false;
        if (need <= rgb_cap) {
            decoded = jpg2rgb565(fb->buf, fb->len, rgb, JPG_SCALE_NONE);
        }
        app_camera_return(fb);

        face_result_t r = {};
        r.frame_id = ++frame_id;
        r.img_w = w; r.img_h = h;

        if (decoded) {
            dl::image::img_t img{};
            img.data     = rgb;
            img.width    = w;
            img.height   = h;
            img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

            auto &results = det->run(img);

            if (!results.empty()) {
                auto best = results.begin();
                for (auto it = results.begin(); it != results.end(); ++it) {
                    if (it->score > best->score) best = it;
                }

                int x0 = best->box[0], y0 = best->box[1];
                int x1 = best->box[2], y1 = best->box[3];
                r.valid = true;
                r.x = x0; r.y = y0;
                r.w = x1 - x0; r.h = y1 - y0;
                r.score = best->score;

                int cx = x0 + r.w / 2;
                int cy = y0 + r.h / 2;
                app_gimbal_track(cx, cy, false);
                miss_streak = 0;
            } else {
                app_gimbal_track(0, 0, true);
                if (miss_streak < 1000) miss_streak++;
            }
        }

        publish_result(r);

        // 每 20 次检测打印一次实测帧率
        det_count++;
        if (det_count % 20 == 0) {
            int64_t now = esp_timer_get_time();
            float fps = 20.0f * 1000000.0f / (float)(now - det_t0);
            ESP_LOGI(TAG, "detect fps=%.2f miss_streak=%u", fps, (unsigned)miss_streak);
            det_t0 = now;
        }

        // G: 自适应间隔——连续无脸时降低检测频率，把 CPU 让给推流
        uint32_t delay_ms = (miss_streak > 30) ? 200 :
                            (miss_streak > 10) ? 60  : 5;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    heap_caps_free(rgb);
    delete det;
    vTaskDelete(NULL);
}

extern "C" esp_err_t app_face_detect_start(void)
{
    s_res_lock = xSemaphoreCreateMutex();
    // F: 优先级 4（低于 httpd/wifi 的 5），核 1，避免抢 httpd
    BaseType_t ok = xTaskCreatePinnedToCore(face_task, "face_det",
                                            8192, NULL, 4, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
