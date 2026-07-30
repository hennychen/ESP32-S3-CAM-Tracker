#include "app_gimbal.h"
#include "camera_pins.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "gimbal";

#define SERVO_FREQ_HZ   50
// ESP32-S3 LEDC LS 分辨率最高 14 位（IDF 5.5 起 LEDC_TIMER_16_BIT 已移除）
#define SERVO_RES_BITS  14
#define SERVO_RES       LEDC_TIMER_14_BIT
#define SERVO_MAX_DUTY  ((1 << SERVO_RES_BITS) - 1)

#define CH_X   LEDC_CHANNEL_2
#define CH_Y   LEDC_CHANNEL_3
#define TIMER  LEDC_TIMER_1
#define SPEED  LEDC_LOW_SPEED_MODE

static float s_ang_x = 90, s_ang_y = 90;
static int   s_img_w = 640, s_img_h = 480;
// PID
static float s_kp = 0.03f, s_kd = 0.01f;
static float s_prev_ex = 0, s_prev_ey = 0;

static uint32_t angle_to_duty(float deg)
{
    if (deg < 0) deg = 0; if (deg > 180) deg = 180;
    float us = 500.0f + 2000.0f * deg / 180.0f;
    return (uint32_t)((us / 20000.0f) * SERVO_MAX_DUTY);
}

static void write_angle(ledc_channel_t ch, float deg)
{
    ledc_set_duty(SPEED, ch, angle_to_duty(deg));
    ledc_update_duty(SPEED, ch);
}

esp_err_t app_gimbal_init(int img_w, int img_h)
{
    s_img_w = img_w; s_img_h = img_h;

    ledc_timer_config_t t = {
        .speed_mode      = SPEED,
        .timer_num       = TIMER,
        .duty_resolution = SERVO_RES,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t cx = {
        .gpio_num=USER_IO_SERVO_X, .speed_mode=SPEED, .channel=CH_X,
        .timer_sel=TIMER, .duty=angle_to_duty(90), .hpoint=0
    };
    ledc_channel_config_t cy = {
        .gpio_num=USER_IO_SERVO_Y, .speed_mode=SPEED, .channel=CH_Y,
        .timer_sel=TIMER, .duty=angle_to_duty(90), .hpoint=0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cx));
    ESP_ERROR_CHECK(ledc_channel_config(&cy));

    ESP_LOGI(TAG, "gimbal ready. SERVO_X=GPIO%d SERVO_Y=GPIO%d", USER_IO_SERVO_X, USER_IO_SERVO_Y);
    return ESP_OK;
}

void app_gimbal_track(int cx, int cy, bool lost)
{
    if (lost) return;

    float ex = (float)(cx - s_img_w / 2);
    float ey = (float)(cy - s_img_h / 2);
    float dx = ex - s_prev_ex, dy = ey - s_prev_ey;
    s_prev_ex = ex; s_prev_ey = ey;

    float out_x = s_kp * ex + s_kd * dx;
    float out_y = s_kp * ey + s_kd * dy;

    // 根据云台安装方向调整正负
    s_ang_x -= out_x * 0.05f;
    s_ang_y += out_y * 0.05f;

    if (s_ang_x < 10) s_ang_x = 10; if (s_ang_x > 170) s_ang_x = 170;
    if (s_ang_y < 30) s_ang_y = 30; if (s_ang_y > 150) s_ang_y = 150;

    write_angle(CH_X, s_ang_x);
    write_angle(CH_Y, s_ang_y);
}
