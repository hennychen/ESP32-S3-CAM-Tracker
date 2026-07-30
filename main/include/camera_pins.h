#pragma once
// ============================================================================
// ESP32-S3-CAM (Type-C / USB-OTG) + OV5640 5MP
// 引脚映射（依据 ESP32-S3CAM 原理图 + 引脚图 ESP32S3CAM-Pin.jpg 提取）
// 如与实际不符，仅需修改本文件。
// ============================================================================

// -------- 电源/复位 --------
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1

// -------- 时钟 --------
#define XCLK_GPIO_NUM     15

// -------- SCCB (I2C for sensor) --------
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5

// -------- 数据总线 D0-D7 --------
#define Y2_GPIO_NUM       11
#define Y3_GPIO_NUM        9
#define Y4_GPIO_NUM        8
#define Y5_GPIO_NUM       10
#define Y6_GPIO_NUM       12
#define Y7_GPIO_NUM       18
#define Y8_GPIO_NUM       17
#define Y9_GPIO_NUM       16

// -------- 同步信号 --------
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

// -------- 板载 & 用户 IO --------
#define LED_BUILTIN_PIN    2
#define USER_IO_SERVO_X   14   // X 轴舵机 PWM
#define USER_IO_SERVO_Y   21   // Y 轴舵机 PWM

// -------- SDMMC --------
#define SD_MMC_CLK        39
#define SD_MMC_CMD        38
#define SD_MMC_D0         40
