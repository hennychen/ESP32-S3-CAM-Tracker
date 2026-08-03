#pragma once
// ============================================================================
// 自动抓拍 & 录像模块
// - 监听人脸检测结果，触发后自动抓拍 1 张照片 + 录制 15s 视频
// - 照片/视频不带任何程序绘制的人脸框，仅叠加时间戳水印
// - 存储到 TF 卡: /sdcard/photo/*.jpg  /sdcard/video/*.avi
// ============================================================================
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动自动录像任务
 *   - 内部初始化 SNTP 时间同步（后台自动同步）
 *   - 检测到人脸后自动抓拍 + 录像
 *   - 需要 TF 卡已挂载（内部检测，未挂载则不启动）
 */
esp_err_t app_record_start(void);

/** @brief 当前是否正在录像 */
bool app_record_is_recording(void);

#ifdef __cplusplus
}
#endif
