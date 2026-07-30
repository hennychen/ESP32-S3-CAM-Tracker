#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化双轴云台（LEDC PWM 输出到 SERVO_X / SERVO_Y） */
esp_err_t app_gimbal_init(int img_w, int img_h);

/** 用最新人脸中心驱动云台；lost=true 时保持 */
void app_gimbal_track(int face_cx, int face_cy, bool lost);

#ifdef __cplusplus
}
#endif
