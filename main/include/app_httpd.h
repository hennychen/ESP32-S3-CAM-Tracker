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
    /* 以下为疲劳监测扩展字段 */
    char     mode[8];          // "online"=上位机在线 / "offline"=ESP32自主监测
    bool     eyes_closed;      // 眼部方差判定闭眼（离线辅助信号）
    float    closed_seconds;   // 连续闭眼/低头秒数
    int      fatigue_level;    // ESP32融合评分等级 0正常 1注意 2疲劳 3危险
    float    fatigue_score;    // 融合评分 0.0~1.0
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

/* ===== 心跳与告警接口（疲劳监测双层架构）===== */

/** 上位机心跳到达（由 /heartbeat handler 调用） */
void app_httpd_heartbeat(void);

/** 上位机是否在线（最近 10s 内有心跳） */
bool app_httpd_is_online(void);

/** 设置告警等级（前端 /alert 或 ESP32 离线自主判定均可调用，取较高值） */
void app_httpd_set_alert(int level);

/** 获取当前告警等级 (0~3) */
int  app_httpd_get_alert(void);

/** 启动 Captive Portal 的 DNS 劫持（仅在 AP 配网模式下调用）
 *  所有 DNS A 查询都返回 192.168.4.1，配合 HTTP 404->302 使手机自动弹出配网页 */
void app_httpd_start_captive_dns(void);

#ifdef __cplusplus
}
#endif
