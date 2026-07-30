// ============================================================================
// TF (microSD) 卡挂载实现 - 1-bit SDMMC 模式
// 硬件接线（见 camera_pins.h）：
//   CLK = GPIO 39
//   CMD = GPIO 38
//   D0  = GPIO 40
// 由于只连接 D0，因此仅支持 1-bit 数据总线模式（速度较慢但兼容性好）。
// ============================================================================
#include "app_sdcard.h"
#include "camera_pins.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "ff.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t app_sdcard_mount(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "mounting TF card (1-bit SDMMC): CLK=%d CMD=%d D0=%d",
             SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

    // FATFS 挂载配置：找不到分区不自动格式化，避免误清空用户数据
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // SDMMC 主机配置（1-bit）
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // 插槽配置：ESP32-S3 支持自定义 GPIO
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SD_MMC_CLK;
    slot_config.cmd = SD_MMC_CMD;
    slot_config.d0  = SD_MMC_D0;
    // 打开内部上拉（大多数模块外部已有上拉，双重保险）
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "mount failed: filesystem may be corrupted or unformatted");
        } else {
            ESP_LOGE(TAG, "mount failed (%s). check card insertion / wiring",
                     esp_err_to_name(ret));
        }
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "TF card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool app_sdcard_is_mounted(void)
{
    return s_mounted;
}

void app_sdcard_unmount(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "TF card unmounted");
}

// ---------------------------------------------------------------------------
// 空间统计 & FIFO 清理
// ---------------------------------------------------------------------------

esp_err_t app_sdcard_get_free_bytes(uint64_t *out_free)
{
    if (!out_free) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FATFS *fs;
    DWORD fre_clust;
    // FATFS 内部路径：使用挂载点前缀带盘号；这里用 "0:" 表示第一个挂载卷
    FRESULT res = f_getfree("0:", &fre_clust, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_getfree failed: %d", res);
        return ESP_FAIL;
    }
    // 一个簇的字节数 = csize * ssize
    uint64_t free_bytes = (uint64_t)fre_clust * fs->csize * FF_MAX_SS;
    *out_free = free_bytes;
    return ESP_OK;
}

// 记录目录内一个候选删除文件（按 mtime 从旧到新排序）
typedef struct {
    char     name[256];   // basename
    time_t   mtime;
    off_t    size;
} sd_file_entry_t;

static int cmp_by_mtime_asc(const void *a, const void *b)
{
    const sd_file_entry_t *fa = (const sd_file_entry_t *)a;
    const sd_file_entry_t *fb = (const sd_file_entry_t *)b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return 0;
}

esp_err_t app_sdcard_ensure_free(const char *dir, uint64_t need_bytes)
{
    if (!dir) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    // 先看当前空间是否已经够
    uint64_t free_bytes = 0;
    esp_err_t err = app_sdcard_get_free_bytes(&free_bytes);
    if (err != ESP_OK) return err;
    if (free_bytes >= need_bytes) return ESP_OK;

    ESP_LOGW(TAG, "space low: free=%llu need=%llu, start FIFO cleanup in %s",
             (unsigned long long)free_bytes, (unsigned long long)need_bytes, dir);

    // 扫描目录，收集候选（普通文件）
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "opendir(%s) failed: %s", dir, strerror(errno));
        return ESP_FAIL;
    }

    size_t cap = 64;
    size_t cnt = 0;
    sd_file_entry_t *list = (sd_file_entry_t *)malloc(cap * sizeof(sd_file_entry_t));
    if (!list) { closedir(d); return ESP_ERR_NO_MEM; }

    struct dirent *ent;
    char path[512];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;   // 跳过 . / ..
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;    // 只删普通文件，不递归子目录

        if (cnt >= cap) {
            size_t new_cap = cap * 2;
            sd_file_entry_t *n = (sd_file_entry_t *)realloc(list, new_cap * sizeof(sd_file_entry_t));
            if (!n) break;
            list = n; cap = new_cap;
        }
        strncpy(list[cnt].name, ent->d_name, sizeof(list[cnt].name) - 1);
        list[cnt].name[sizeof(list[cnt].name) - 1] = '\0';
        list[cnt].mtime = st.st_mtime;
        list[cnt].size  = st.st_size;
        cnt++;
    }
    closedir(d);

    if (cnt == 0) {
        free(list);
        ESP_LOGW(TAG, "no regular files in %s to delete", dir);
        return ESP_ERR_NOT_FOUND;
    }

    // 按修改时间升序（最早的先删）
    qsort(list, cnt, sizeof(sd_file_entry_t), cmp_by_mtime_asc);

    size_t deleted = 0;
    uint64_t reclaimed = 0;
    for (size_t i = 0; i < cnt; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, list[i].name);
        if (unlink(path) == 0) {
            deleted++;
            reclaimed += (uint64_t)list[i].size;
            ESP_LOGI(TAG, "deleted[%zu]: %s (%ld bytes, mtime=%ld)",
                     deleted, path, (long)list[i].size, (long)list[i].mtime);
        } else {
            ESP_LOGW(TAG, "unlink(%s) failed: %s", path, strerror(errno));
            continue;
        }

        // 每删若干个再复核一次 f_getfree，减少 IO 开销
        if ((deleted & 0x03) == 0) {
            if (app_sdcard_get_free_bytes(&free_bytes) == ESP_OK &&
                free_bytes >= need_bytes) {
                break;
            }
        }
    }

    free(list);

    // 最终确认
    err = app_sdcard_get_free_bytes(&free_bytes);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "cleanup done: deleted=%zu, reclaimed=%llu, free=%llu (need=%llu)",
             deleted, (unsigned long long)reclaimed,
             (unsigned long long)free_bytes, (unsigned long long)need_bytes);

    if (free_bytes < need_bytes) {
        ESP_LOGW(TAG, "still not enough space after cleanup");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
