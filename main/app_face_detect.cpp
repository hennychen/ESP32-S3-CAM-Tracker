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
#include <math.h>
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
    // MSR 原始分数偏低但 MNP 精调后可达 1.0，故 MSR 放宽、MNP 收紧
    det->set_score_thr(0.05, 0);   // MSR 阶段：放宽门槛让候选进入 MNP
    det->set_score_thr(0.5,  1);   // MNP 阶段：最终筛选，score>0.5 才算有效人脸

    // C: 预分配 RGB 缓冲（按 HVGA 480x320 上限，兼容更大分辨率时可下调）
    const size_t rgb_cap = 640 * 480 * 2;
    uint8_t *rgb = (uint8_t *)heap_caps_malloc(rgb_cap, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "no PSRAM for RGB buffer");
        vTaskDelete(NULL); return;
    }

    // 摄像头预热：丢弃前 12 帧让 OV5640 AE/AWB 收敛，避免初期暗帧导致漏检
    for (int i = 0; i < 12; i++) {
        camera_fb_t *wb = app_camera_get();
        if (wb) app_camera_return(wb);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    ESP_LOGI(TAG, "camera warmup done");

    uint32_t frame_id = 0;
    uint32_t miss_streak = 0;   // 连续未检出人脸的帧数
    uint32_t det_count  = 0;
    int64_t  det_t0 = esp_timer_get_time();

    // P3: EMA 平滑器——消除帧间坐标跳变
    // alpha 越大越灵敏（0.5 = 新旧各半）；跳变 >80px 时重置（换人/误检）
    const float ema_alpha = 0.5f;
    bool  ema_active = false;
    float ema_cx = 0, ema_cy = 0, ema_w = 0, ema_h = 0;

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
                int raw_cx = x0 + (x1 - x0) / 2;
                int raw_cy = y0 + (y1 - y0) / 2;
                int raw_w = x1 - x0, raw_h = y1 - y0;

                // P3: EMA 平滑（跳变 >80px 时重置跟踪目标）
                if (ema_active) {
                    int dx = raw_cx - (int)ema_cx;
                    int dy = raw_cy - (int)ema_cy;
                    if (dx*dx + dy*dy > 80*80) {
                        ema_active = false;  // 大跳变=换人，重置
                    }
                }
                if (!ema_active) {
                    ema_cx = raw_cx; ema_cy = raw_cy;
                    ema_w = raw_w;   ema_h = raw_h;
                    ema_active = true;
                } else {
                    ema_cx = ema_alpha * raw_cx + (1-ema_alpha) * ema_cx;
                    ema_cy = ema_alpha * raw_cy + (1-ema_alpha) * ema_cy;
                    ema_w  = ema_alpha * raw_w  + (1-ema_alpha) * ema_w;
                    ema_h  = ema_alpha * raw_h  + (1-ema_alpha) * ema_h;
                }

                r.valid = true;
                r.x = (int)ema_cx - (int)ema_w / 2;
                r.y = (int)ema_cy - (int)ema_h / 2;
                r.w = (int)ema_w;
                r.h = (int)ema_h;
                r.score = best->score;

                // 用平滑后的中心驱动云台（减少抖动）
                app_gimbal_track((int)ema_cx, (int)ema_cy, false);
                miss_streak = 0;

                // 读取 5 点关键点（MNP 输出）并计算粗略姿态指标
                // 点序：[0]左眼 [1]左嘴角 [2]鼻尖 [3]右眼 [4]右嘴角
                if (best->keypoint.size() >= 10) {
                    for (int i = 0; i < 10; i++) r.kp[i] = best->keypoint[i];

                    float ley = r.kp[1], rey = r.kp[7];   // 左/右眼 y
                    float lmy = r.kp[3], rmy = r.kp[9];   // 左/右嘴角 y
                    float ney = r.kp[5];                   // 鼻尖 y
                    float lex = r.kp[0], rex = r.kp[6];   // 左/右眼 x

                    // 头部倾斜角（roll）：双眼连线与水平线夹角
                    r.roll = atan2f(rey - ley, rex - lex) * 180.0f / M_PI;

                    // 眼→鼻 / 眼→嘴 垂直比例（正常约 0.5，低头时下降）
                    float emy = (ley + rey) * 0.5f;
                    float mmy = (lmy + rmy) * 0.5f;
                    float eye_mouth = mmy - emy;
                    r.vert_ratio = (eye_mouth > 1.0f) ? (ney - emy) / eye_mouth : 0.5f;

                    // 粗略疲劳判断：低头或歪头
                    r.drowsy = (r.vert_ratio < 0.35f) || (fabsf(r.roll) > 25.0f);
                }

                ESP_LOGI(TAG, "face: score=%.2f box=[%d,%d,%d,%d] raw=[%d,%d,%d,%d] roll=%.0f vr=%.2f drowsy=%d #%u",
                         best->score,
                         r.x, r.y, r.x+r.w, r.y+r.h,
                         x0, y0, x1, y1,
                         r.roll, r.vert_ratio, r.drowsy ? 1 : 0, (unsigned)frame_id);
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

        // P0: 检测延迟降低（原 100/200ms → 50/150ms）
        // /capture 短轮询替代了 /stream 长连接，帧缓冲竞争大幅减少
        uint32_t delay_ms = (miss_streak > 10) ? 150 : 50;
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
