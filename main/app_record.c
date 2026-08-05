// ============================================================================
// 自动抓拍 & 录像模块
// ============================================================================
#include "app_record.h"
#include "app_sdcard.h"
#include "app_face_detect.h"
#include "app_camera.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "record";

// ---- 配置 ----
#define REC_DURATION_SEC   15      // 视频时长
#define REC_FPS            10      // 目标帧率
#define REC_COOLDOWN_SEC   30      // 两次录制最小间隔（冷却）
#define PHOTO_DIR          "/sdcard/photo"
#define VIDEO_DIR          "/sdcard/video"
#define RGB_BUF_CAP        (640 * 480 * 2)

// ====================================================================
// 5x7 点阵字体（仅含时间戳所需字符）
// 每字符 7 行，每行 5 bit（bit4=最左列）
// ====================================================================
typedef struct { char ch; const uint8_t data[7]; } glyph_t;

static const glyph_t s_font[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};
#define FONT_COUNT (sizeof(s_font)/sizeof(s_font[0]))

static const uint8_t *get_glyph(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;   // 转大写
    for (int i = 0; i < (int)FONT_COUNT; i++) {
        if (s_font[i].ch == c) return s_font[i].data;
    }
    return s_font[0].data;   // 缺省=空格
}

// ====================================================================
// 文字绘制（RGB565LE 缓冲）
// ====================================================================
static void draw_text_rgb565(uint16_t *buf, int w, int h,
                             int x, int y, const char *str,
                             int scale, uint16_t fg, uint16_t bg)
{
    int cx = x;
    for (const char *p = str; *p; p++) {
        const uint8_t *g = get_glyph(*p);
        for (int r = 0; r < 7; r++) {
            uint8_t row = g[r];
            for (int c = 0; c < 5; c++) {
                uint16_t color = (row & (0x10 >> c)) ? fg : bg;
                if (color == 0xFFFF) continue;  // 透明背景
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = cx + c*scale + sx;
                        int py = y + r*scale + sy;
                        if (px >= 0 && px < w && py >= 0 && py < h)
                            buf[(size_t)py * w + px] = color;
                    }
                }
            }
        }
        cx += 6 * scale;   // 5 像素字宽 + 1 像素间距
    }
}

static void draw_rect_fill_rgb565(uint16_t *buf, int w, int h,
                                  int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            buf[(size_t)y * w + x] = color;
}

// ====================================================================
// 时间戳生成
// ====================================================================
static void format_timestamp(char *out, size_t outsz)
{
    time_t now = time(NULL);
    if (now > 1700000000) {     // 已有有效时间（2023 年后）
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(out, outsz, "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        // 未同步，使用启动后秒数
        int sec = (int)(esp_timer_get_time() / 1000000);
        snprintf(out, outsz, "BOOT+%04ds", sec);
    }
}

static void make_filename(char *out, size_t outsz, bool is_photo)
{
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        char base[32];
        strftime(base, sizeof(base), "%Y%m%d_%H%M%S", &tm);
        snprintf(out, outsz, "%s_%s.%s",
                 is_photo ? "IMG" : "VID", base,
                 is_photo ? "jpg" : "avi");
    } else {
        int sec = (int)(esp_timer_get_time() / 1000000);
        snprintf(out, outsz, "%s_BOOT_%06d.%s",
                 is_photo ? "IMG" : "VID", sec,
                 is_photo ? "jpg" : "avi");
    }
}

// ====================================================================
// JPEG 时间戳水印：解码 -> 画时间戳 -> 重新编码
// jpg_in: 原始干净 JPEG（不带人脸框）
// 返回 jpg_out（调用方 free），失败返回 false
// ====================================================================
static bool burn_timestamp(const uint8_t *jpg_in, size_t jpg_len,
                           uint8_t *rgb_buf, size_t rgb_cap,
                           const char *ts_str, bool show_rec,
                           uint8_t **jpg_out, size_t *jpg_out_len)
{
    int w, h;
    // 先解析 JPEG 尺寸
    esp_jpeg_image_cfg_t jcfg = {
        .indata = (uint8_t *)jpg_in,
        .indata_size = jpg_len,
        .outbuf = rgb_buf,
        .outbuf_size = rgb_cap,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t jout = {};
    if (esp_jpeg_decode(&jcfg, &jout) != ESP_OK) return false;
    w = jout.width; h = jout.height;

    uint16_t *px = (uint16_t *)rgb_buf;

    // 底部黑色背景条 + 时间戳白字
    int scale = (w >= 400) ? 2 : 1;
    int ts_h  = 7 * scale + 4;
    int ts_y  = h - ts_h - 2;
    draw_rect_fill_rgb565(px, w, h, 0, ts_y, w-1, h-1, 0x0000);
    draw_text_rgb565(px, w, h, 4, ts_y + 2, ts_str, scale, 0xFFFF, 0xFFFF);

    // 录像时左上角红色 REC 标识
    if (show_rec) {
        draw_rect_fill_rgb565(px, w, h, 0, 0, 12*scale + 4, 7*scale + 4, 0x0000);
        draw_text_rgb565(px, w, h, 2, 2, "REC", scale, 0xF800, 0xFFFF);
    }

    // RGB565 -> JPEG
    jpgSetRgb565BE(false);
    size_t need = (size_t)w * h * 2;
    return fmt2jpg(rgb_buf, need, w, h, PIXFORMAT_RGB565, 15,
                   jpg_out, jpg_out_len);
}

// ====================================================================
// AVI (MJPEG) 写入器
// ====================================================================
typedef struct {
    FILE *fp;
    int width, height, fps;
    uint32_t frame_count;
    long riff_size_pos;    // RIFF size 字段偏移
    long total_frames_pos; // avih.dwTotalFrames 偏移
    long strh_length_pos;  // strh.dwLength 偏移
    long movi_size_pos;    // movi LIST size 偏移
    long movi_data_start;  // 第一帧在文件中的偏移
    // 索引
    uint32_t *idx_off;
    uint32_t *idx_len;
    uint32_t idx_cap;
} avi_writer_t;

static void put_u32_at(FILE *f, long pos, uint32_t v)
{
    long cur = ftell(f);
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24)};
    fseek(f, pos, SEEK_SET);
    fwrite(b, 1, 4, f);
    fseek(f, cur, SEEK_SET);
}

static void put_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24)};
    fwrite(b, 1, 4, f);
}

static void put_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v>>8)};
    fwrite(b, 1, 2, f);
}

static void put_tag(FILE *f, const char *t)
{
    fwrite(t, 1, 4, f);
}

static avi_writer_t *avi_open(const char *path, int w, int h, int fps)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "avi open fail: %s", path);
        return NULL;
    }

    avi_writer_t *a = calloc(1, sizeof(avi_writer_t));
    a->fp = fp; a->width = w; a->height = h; a->fps = fps;

    // idx 缓冲（预分配上限 600 帧）
    a->idx_cap = 600;
    a->idx_off = calloc(a->idx_cap, sizeof(uint32_t));
    a->idx_len = calloc(a->idx_cap, sizeof(uint32_t));

    // ---- RIFF + AVI ----
    put_tag(fp, "RIFF");
    a->riff_size_pos = ftell(fp);
    put_u32(fp, 0);              // placeholder
    put_tag(fp, "AVI ");

    // ---- hdrl LIST ----
    // size = "hdrl"(4) + avih_chunk(64) + strl_LIST(124) = 192
    put_tag(fp, "LIST");
    put_u32(fp, 192);
    put_tag(fp, "hdrl");

    // ---- avih (56 bytes) ----
    put_tag(fp, "avih");
    put_u32(fp, 56);
    put_u32(fp, 1000000 / fps);  // dwMicroSecPerFrame
    put_u32(fp, 500000);          // dwMaxBytesPerSec
    put_u32(fp, 0);               // dwPaddingGranularity
    put_u32(fp, 0x10);            // dwFlags = AVIF_HASINDEX
    a->total_frames_pos = ftell(fp);
    put_u32(fp, 0);               // dwTotalFrames (patch)
    put_u32(fp, 0);               // dwInitialFrames
    put_u32(fp, 1);               // dwStreams
    put_u32(fp, 0);               // dwSuggestedBufferSize
    put_u32(fp, w);               // dwWidth
    put_u32(fp, h);               // dwHeight
    put_u32(fp, 0); put_u32(fp, 0); put_u32(fp, 0); put_u32(fp, 0); // reserved

    // ---- strl LIST ----
    // size = "strl"(4) + strh_chunk(64) + strf_chunk(48) = 116
    put_tag(fp, "LIST");
    put_u32(fp, 116);
    put_tag(fp, "strl");

    // strh (56 bytes)
    put_tag(fp, "strh");
    put_u32(fp, 56);
    put_tag(fp, "vids");           // fccType
    put_tag(fp, "mjpg");           // fccHandler
    put_u32(fp, 0);               // dwFlags
    put_u32(fp, 0);               // wPriority + wLanguage
    put_u32(fp, 0);               // dwInitialFrames
    put_u32(fp, 1);               // dwScale
    put_u32(fp, fps);             // dwRate
    put_u32(fp, 0);               // dwStart
    a->strh_length_pos = ftell(fp);
    put_u32(fp, 0);               // dwLength (patch)
    put_u32(fp, 0);               // dwSuggestedBufferSize
    put_u32(fp, 0xFFFFFFFF);      // dwQuality
    put_u32(fp, 0);               // dwSampleSize
    put_u32(fp, 0);               // rcFrame.left
    put_u32(fp, 0);               // rcFrame.top
    put_u32(fp, (uint32_t)w);     // rcFrame.right
    put_u32(fp, (uint32_t)h);     // rcFrame.bottom

    // strf (BITMAPINFOHEADER, 40 bytes)
    put_tag(fp, "strf");
    put_u32(fp, 40);
    put_u32(fp, 40);              // biSize
    put_u32(fp, (uint32_t)w);     // biWidth
    put_u32(fp, (uint32_t)h);     // biHeight
    put_u16(fp, 1);               // biPlanes
    put_u16(fp, 24);              // biBitCount
    put_tag(fp, "mjpg");          // biCompression
    put_u32(fp, (uint32_t)(w*h*3)); // biSizeImage
    put_u32(fp, 0); put_u32(fp, 0); put_u32(fp, 0); put_u32(fp, 0);

    // ---- movi LIST ----
    put_tag(fp, "LIST");
    a->movi_size_pos = ftell(fp);
    put_u32(fp, 0);               // placeholder
    put_tag(fp, "movi");
    a->movi_data_start = ftell(fp);

    return a;
}

static bool avi_add_frame(avi_writer_t *a, const uint8_t *jpeg, size_t len)
{
    if (!a || !a->fp) return false;
    if (a->frame_count >= a->idx_cap) return false;

    long chunk_pos = ftell(a->fp);
    put_tag(a->fp, "00dc");
    put_u32(a->fp, (uint32_t)len);
    fwrite(jpeg, 1, len, a->fp);
    // JPEG 帧数据按偶数对齐（AVI 规范）
    if (len & 1) { uint8_t z=0; fwrite(&z, 1, 1, a->fp); }

    a->idx_off[a->frame_count] = (uint32_t)(chunk_pos - a->movi_data_start + 4);
    a->idx_len[a->frame_count] = (uint32_t)len;
    a->frame_count++;
    return true;
}

static void avi_close(avi_writer_t *a)
{
    if (!a) return;
    FILE *f = a->fp;

    // ---- idx1 ----
    long idx_start = ftell(f);
    put_tag(f, "idx1");
    put_u32(f, a->frame_count * 16);
    for (uint32_t i = 0; i < a->frame_count; i++) {
        put_tag(f, "00dc");
        put_u32(f, 0x10);             // AVIIF_KEYFRAME
        put_u32(f, a->idx_off[i]);
        put_u32(f, a->idx_len[i]);
    }
    long file_end = ftell(f);
    uint32_t fc = a->frame_count;

    // ---- 回填所有 size 字段 ----
    uint32_t movi_size = (uint32_t)(idx_start - (a->movi_size_pos + 4));
    put_u32_at(f, a->movi_size_pos, movi_size);
    put_u32_at(f, a->riff_size_pos, (uint32_t)(file_end - 8));
    put_u32_at(f, a->total_frames_pos, fc);
    put_u32_at(f, a->strh_length_pos, fc);

    fclose(f);
    free(a->idx_off);
    free(a->idx_len);
    free(a);
    ESP_LOGI(TAG, "avi closed: %u frames", (unsigned)fc);
}

// ====================================================================
// SNTP 初始化
// ====================================================================
static void sntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synced: %lld", (long long)tv->tv_sec);
}

static void init_sntp(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized (CST-8)");
}

// ====================================================================
// 录像任务
// ====================================================================
static SemaphoreHandle_t s_rec_lock;
static bool s_recording = false;
static bool s_sd_ok = false;

bool app_record_is_recording(void)
{
    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    bool v = s_recording;
    xSemaphoreGive(s_rec_lock);
    return v;
}

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0777);
    }
}

/** 抓拍一张照片（干净 JPEG + 时间戳水印） */
static void capture_photo(uint8_t *rgb_buf, size_t rgb_cap)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { ESP_LOGW(TAG, "photo: fb fail"); return; }

    char ts[40];
    format_timestamp(ts, sizeof(ts));

    uint8_t *jpg_out = NULL;
    size_t jpg_len = 0;
    if (burn_timestamp(fb->buf, fb->len, rgb_buf, rgb_cap, ts, false,
                       &jpg_out, &jpg_len)) {
        char fname[64];
        make_filename(fname, sizeof(fname), true);
        char fpath[128];
        snprintf(fpath, sizeof(fpath), "%s/%s", PHOTO_DIR, fname);
        FILE *f = fopen(fpath, "wb");
        if (f) {
            fwrite(jpg_out, 1, jpg_len, f);
            fclose(f);
            ESP_LOGI(TAG, "photo saved: %s (%u bytes) [%s]", fpath,
                     (unsigned)jpg_len, ts);
        } else {
            ESP_LOGE(TAG, "photo write fail: %s", fpath);
        }
        free(jpg_out);
    }
    esp_camera_fb_return(fb);
}

/** 录制 REC_DURATION_SEC 秒 MJPEG AVI（每帧叠加时间戳水印） */
static void record_video(uint8_t *rgb_buf, size_t rgb_cap)
{
    char fname[64];
    make_filename(fname, sizeof(fname), false);
    char fpath[128];
    snprintf(fpath, sizeof(fpath), "%s/%s", VIDEO_DIR, fname);

    camera_fb_t *probe = esp_camera_fb_get();
    int w = probe ? probe->width : 320;
    int h = probe ? probe->height : 240;
    if (probe) esp_camera_fb_return(probe);

    avi_writer_t *avi = avi_open(fpath, w, h, REC_FPS);
    if (!avi) return;

    int64_t t_start = esp_timer_get_time();
    int64_t duration_us = REC_DURATION_SEC * 1000000LL;
    uint32_t frame_interval_us = 1000000 / REC_FPS;
    uint32_t saved = 0;

    while (esp_timer_get_time() - t_start < duration_us) {
        int64_t target = t_start + saved * frame_interval_us;
        int64_t now = esp_timer_get_time();
        if (now < target) {
            vTaskDelay(pdMS_TO_TICKS((target - now) / 1000));
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) continue;

        char ts[40];
        format_timestamp(ts, sizeof(ts));

        uint8_t *jpg_out = NULL;
        size_t jpg_len = 0;
        if (burn_timestamp(fb->buf, fb->len, rgb_buf, rgb_cap, ts, true,
                           &jpg_out, &jpg_len)) {
            if (!avi_add_frame(avi, jpg_out, jpg_len)) {
                free(jpg_out);
                break;
            }
            free(jpg_out);
            saved++;
        }
        esp_camera_fb_return(fb);
    }

    avi_close(avi);
    ESP_LOGI(TAG, "video saved: %s (%u frames)", fpath, (unsigned)saved);
}

static void record_task(void *arg)
{
    ESP_LOGI(TAG, "record task started (sd=%s)", s_sd_ok ? "ok" : "FAIL");

    if (!s_sd_ok) { vTaskDelete(NULL); return; }

    ensure_dir(PHOTO_DIR);
    ensure_dir(VIDEO_DIR);

    // 预分配 RGB 工作缓冲
    uint8_t *rgb = (uint8_t *)heap_caps_malloc(RGB_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "no PSRAM for RGB buf");
        vTaskDelete(NULL); return;
    }

    uint32_t miss_streak = 0;
    int64_t last_rec_end = 0;
    uint32_t poll_tick = 0;

    while (true) {
        face_result_t f;
        app_face_detect_get(&f);

        if (f.valid) {
            miss_streak = 0;
            // 检查冷却
            int64_t now = esp_timer_get_time();
            bool cooldown_ok = (last_rec_end == 0) ||
                               (now - last_rec_end > REC_COOLDOWN_SEC * 1000000LL);

            if (cooldown_ok) {
                // 确认连续检测到（防误触发）：连续 2 次有效
                vTaskDelay(pdMS_TO_TICKS(200));
                app_face_detect_get(&f);
                if (f.valid) {
                    ESP_LOGI(TAG, ">>> face detected, start recording session");

                    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
                    s_recording = true;
                    xSemaphoreGive(s_rec_lock);

                    // 先确保空间（预留 5MB）
                    app_sdcard_ensure_free(PHOTO_DIR, 200 * 1024);
                    app_sdcard_ensure_free(VIDEO_DIR, 5 * 1024 * 1024);

                    capture_photo(rgb, RGB_BUF_CAP);
                    record_video(rgb, RGB_BUF_CAP);

                    last_rec_end = esp_timer_get_time();
                    ESP_LOGI(TAG, "<<< recording session done");

                    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
                    s_recording = false;
                    xSemaphoreGive(s_rec_lock);
                }
            }
        } else {
            miss_streak++;
        }

        // 每 10 秒打印一次状态
        poll_tick++;
        if (poll_tick % 20 == 0) {
            ESP_LOGI(TAG, "monitor: face=%s miss=%u recording=%s",
                     f.valid ? "Y" : "N", (unsigned)miss_streak,
                     s_recording ? "Y" : "N");
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
    heap_caps_free(rgb);
    vTaskDelete(NULL);
}

esp_err_t app_record_start(void)
{
    s_rec_lock = xSemaphoreCreateMutex();

    // 尝试挂载 TF 卡
    s_sd_ok = app_sdcard_is_mounted();
    if (!s_sd_ok) {
        esp_err_t err = app_sdcard_mount();
        s_sd_ok = (err == ESP_OK);
    }

    if (s_sd_ok) {
        ESP_LOGI(TAG, "TF card ready for recording");
        init_sntp();
    } else {
        ESP_LOGW(TAG, "TF card not available, recording disabled");
    }

    // 核 1，优先级 2（低于 face_det 的 4 和 httpd 的 5）
    BaseType_t ok = xTaskCreatePinnedToCore(record_task, "record",
                                            12288, NULL, 2, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
