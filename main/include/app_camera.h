#pragma once
#include "esp_camera.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 摄像头初始化。frame_size 参考 sensor.h 的 framesize_t（推荐 FRAMESIZE_VGA） */
esp_err_t app_camera_init(framesize_t frame_size, pixformat_t pixel_format);

/** 抓帧/释放帧 */
camera_fb_t *app_camera_get(void);
void          app_camera_return(camera_fb_t *fb);

/** 切换分辨率 */
esp_err_t app_camera_set_framesize(framesize_t s);

#ifdef __cplusplus
}
#endif
