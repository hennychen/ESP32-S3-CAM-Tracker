#pragma once
// ============================================================================
// TF (microSD) 卡挂载 - 1-bit SDMMC 模式
// 引脚映射见 camera_pins.h : SD_MMC_CLK / SD_MMC_CMD / SD_MMC_D0
// ============================================================================
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"

/**
 * @brief 初始化并挂载 TF 卡（1-bit SDMMC，FATFS -> /sdcard）
 * @return ESP_OK 成功；否则表示卡未插入或初始化失败
 */
esp_err_t app_sdcard_mount(void);

/** @brief 是否已成功挂载 */
bool app_sdcard_is_mounted(void);

/** @brief 卸载 SD 卡 */
void app_sdcard_unmount(void);

/**
 * @brief 获取 TF 卡剩余可用字节数
 * @param out_free  [out] 剩余字节数
 * @return ESP_OK 成功
 */
esp_err_t app_sdcard_get_free_bytes(uint64_t *out_free);

/**
 * @brief 确保指定目录所在文件系统至少有 need_bytes 空闲空间；
 *        不足时按 "先存先删" (FIFO, 按文件 mtime 从旧到新) 依次删除该目录下的
 *        普通文件，直到空间满足或目录为空。
 *
 * 用法示例：写图片前先调用 app_sdcard_ensure_free("/sdcard/photo", 512*1024);
 *
 * @param dir         需要清理的目录（例如 "/sdcard/photo"）
 * @param need_bytes  期望的最小可用空间（字节）
 * @return ESP_OK      空间充足或清理后满足
 *         ESP_ERR_NOT_FOUND  目录已空但仍不足
 *         其它错误码 表示统计或删除失败
 */
esp_err_t app_sdcard_ensure_free(const char *dir, uint64_t need_bytes);

#ifdef __cplusplus
}
#endif
