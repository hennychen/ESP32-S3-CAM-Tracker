#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 最近一次人脸检测结果（供 httpd 叠加到 web 页面 / 云台使用） */
typedef struct {
    bool     valid;
    int      x, y, w, h;    // 人脸框（图像原始坐标）
    float    score;
    uint32_t frame_id;
    int      img_w, img_h;  // 该帧的图像尺寸
    /* 5 点关键点（MNP 输出，原图绝对坐标）
     * [0]左眼 [1]左嘴角 [2]鼻尖 [3]右眼 [4]右嘴角 */
    int      kp[10];        // x0,y0,x1,y1,...,x4,y4
    float    roll;          // 头部倾斜角（度，正值=右倾）
    float    vert_ratio;    // 眼-鼻 / 眼-嘴 垂直比例（正常~0.5，低头时下降）
    bool     drowsy;        // 粗略疲劳标志（低头/歪头）
} face_result_t;

/** 启动 HTTP MJPEG 服务器：
 *   /            主页（HTML）
 *   /stream      multipart/x-mixed-replace  MJPEG 推流
 *   /face        最新人脸检测结果 JSON
 *   /capture     单帧 JPEG
 */
esp_err_t app_httpd_start(void);

/** 由人脸检测线程回写最新结果，供 /face 与前端使用 */
void app_httpd_set_face(const face_result_t *r);

/** 启动 Captive Portal 的 DNS 劫持（仅在 AP 配网模式下调用）
 *  所有 DNS A 查询都返回 192.168.4.1，配合 HTTP 404->302 使手机自动弹出配网页 */
void app_httpd_start_captive_dns(void);

#ifdef __cplusplus
}
#endif
