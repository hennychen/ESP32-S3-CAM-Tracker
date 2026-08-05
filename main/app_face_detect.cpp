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

// 计算眼部区域灰度方差（开眼方差大：瞳孔+眼白纹理；闭眼方差小：眼皮均匀）
static float calc_eye_var(const uint8_t *rgb565, int img_w, int img_h,
                          int cx, int cy, int rad)
{
    int x0 = cx - rad; if (x0 < 0) x0 = 0;
    int x1 = cx + rad; if (x1 > img_w) x1 = img_w;
    int y0 = cy - rad; if (y0 < 0) y0 = 0;
    int y1 = cy + rad; if (y1 > img_h) y1 = img_h;
    int count = 0;
    float sum = 0, sum_sq = 0;
    const uint16_t *px16 = (const uint16_t *)rgb565;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint16_t px = px16[y * img_w + x];
            uint8_t g = (px >> 5) & 0x3F;  // 绿通道（6bit，近似亮度）
            sum += g;
            sum_sq += (float)g * g;
            count++;
        }
    }
    if (count < 4) return 0;
    float mean = sum / count;
    return sum_sq / count - mean * mean;
}

static void face_task(void *arg)
{
    ESP_LOGI(TAG, "face detect task start (decoupled from stream)");
    HumanFaceDetect *det = new HumanFaceDetect();
    // MSR 原始分数偏低但 MNP 精调后可达 1.0，故 MSR 放宽、MNP 收紧
    det->set_score_thr(0.05, 0);   // MSR 阶段：放宽门槛让候选进入 MNP
    det->set_score_thr(0.5,  1);   // MNP 阶段：最终筛选，score>0.5 才算有效人脸

    // C: 预分配 RGB 缓冲（VGA 640x480 双字节 = 614400 字节）
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

    // 疲劳监测时序状态
    float vr_hist[40];          // vert_ratio 环形缓冲（~2s @ 50ms/帧）
    int vr_hist_n = 0, vr_hist_head = 0;
    float eye_var_baseline = -1; // 眼部方差基线（前30帧EMA）
    int eye_var_calib = 0;
    uint32_t closed_frames = 0;  // 连续闭眼/低头帧数

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

        float eye_var = -1, vr_avg = 0;
        bool eyes_closed = false;

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

                // P3: EMA 平滑（跳变 >160px 时重置跟踪目标，VGA 分辨率下按比例放大）
                if (ema_active) {
                    int dx = raw_cx - (int)ema_cx;
                    int dy = raw_cy - (int)ema_cy;
                    if (dx*dx + dy*dy > 160*160) {
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

                // 眼部区域方差（辅助信号：闭眼时纹理减少→方差下降）
                if (r.w > 20 && r.kp[0] > 0 && r.kp[6] > 0) {
                    int eye_dist = abs(r.kp[6] - r.kp[0]);
                    if (eye_dist > 8) {
                        int rad = eye_dist / 5;
                        float lv = calc_eye_var(rgb, w, h, r.kp[0], r.kp[1], rad);
                        float rv = calc_eye_var(rgb, w, h, r.kp[6], r.kp[7], rad);
                        eye_var = (lv + rv) * 0.5f;
                        // 构建基线（前30帧 EMA）
                        if (eye_var_calib < 30) {
                            eye_var_baseline = (eye_var_baseline < 0) ? eye_var
                                : eye_var_baseline * 0.9f + eye_var * 0.1f;
                            eye_var_calib++;
                        }
                    }
                }

                // 头部姿态时序：vert_ratio 环形缓冲平滑
                vr_hist[vr_hist_head] = r.vert_ratio;
                vr_hist_head = (vr_hist_head + 1) % 40;
                if (vr_hist_n < 40) vr_hist_n++;
                int vn = (vr_hist_n < 20) ? vr_hist_n : 20;
                for (int i = 0; i < vn; i++)
                    vr_avg += vr_hist[(vr_hist_head - vn + i + 40) % 40];
                if (vn > 0) vr_avg /= vn;

                // 眼部方差判定闭眼（方差降至基线35%以下）
                if (eye_var >= 0 && eye_var_baseline > 0 && eye_var < eye_var_baseline * 0.35f)
                    eyes_closed = true;

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

        // === ESP32 端疲劳融合评分（始终运行，前端离线时自主告警）===
        {
            if (eyes_closed || r.drowsy) closed_frames++;
            else closed_frames = 0;

            float score = 0;
            // 信号1：头部下垂（平滑后的 vert_ratio）
            if (vr_avg > 0) {
                if (vr_avg < 0.28f) score += 0.4f;       // 严重低头
                else if (vr_avg < 0.35f) score += 0.2f;   // 轻微低头
            }
            // 信号2：头部歪斜
            if (r.valid && fabsf(r.roll) > 20.0f) score += 0.2f;
            // 信号3：持续丢脸（>2s 无脸 = 头完全低垂）
            if (miss_streak > 40) score += 0.4f;
            // 信号4：眼部方差下降
            if (eyes_closed) score += 0.15f;
            // 信号5：原有粗略疲劳标志
            if (r.drowsy) score += 0.15f;

            score = fminf(score, 1.0f);
            int level = (score >= 0.7f) ? 3 : (score >= 0.5f) ? 2 : (score >= 0.3f) ? 1 : 0;

            bool online = app_httpd_is_online();
            snprintf(r.mode, sizeof(r.mode), "%s", online ? "online" : "offline");
            r.eyes_closed = eyes_closed;
            r.closed_seconds = closed_frames * 0.05f;
            r.fatigue_score = score;
            r.fatigue_level = level;

            // 离线模式：ESP32 自主触发告警
            if (!online) app_httpd_set_alert(level);
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
