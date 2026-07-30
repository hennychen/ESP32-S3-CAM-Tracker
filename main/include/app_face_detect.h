#pragma once
#include "esp_err.h"
#include "app_httpd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动人脸检测任务（内部抓帧，并把 JPEG 共享给 HTTP 推流） */
esp_err_t app_face_detect_start(void);

/** HTTP /stream 使用：获取最新一帧 JPEG（不 malloc；调用方使用完必须 give_sem 释放） */
bool app_face_detect_frame_lock(const uint8_t **buf, size_t *len);
void app_face_detect_frame_unlock(void);

/** 最新一次检测结果 */
void app_face_detect_get(face_result_t *out);

#ifdef __cplusplus
}
#endif
