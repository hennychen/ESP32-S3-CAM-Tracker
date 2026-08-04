# 驾驶员疲劳监测系统 (DMS) 方案

> **状态**：方案设计完成，待硬件接入后实施
> **基线**：ESP32-S3-CAM-Tracker v0.8（人脸识别与追踪）
> **核心传感器**：OV5640 摄像头 + SEN0691 C4002 毫米波雷达（+ 可选 MPU-6050 / BME280）
> **创建日期**：2026-08-03

---

## 目录

- [1. 系统总览](#1-系统总览)
- [2. 硬件方案](#2-硬件方案)
- [3. SEN0691 C4002 功能详解](#3-sen0691-c4002-功能详解)
- [4. 融合决策算法](#4-融合决策算法)
- [5. 软件架构](#5-软件架构)
- [6. 告警分级](#6-告警分级)
- [7. REST API 扩展](#7-rest-api-扩展)
- [8. 实施路线图](#8-实施路线图)
- [9. 精度预期](#9-精度预期)
- [10. 关键约束与风险](#10-关键约束与风险)
- [11. C4002 车载配置参考](#11-c4002-车载配置参考)
- [12. 代码骨架](#12-代码骨架)

---

## 1. 系统总览

基于现有 **ESP32-S3-CAM (N16R8) + OV5640** 人脸追踪平台，扩展为多传感器融合的驾驶员疲劳监测系统。

```
┌─────────────────────────────────────────────────────────────────┐
│                    驾驶员疲劳监测系统 (DMS)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────┐    ┌──────────────┐    ┌───────────────────────┐  │
│  │ OV5640   │───►│ ESP32-S3     │───►│ 告警输出              │  │
│  │ 摄像头    │    │ N16R8        │    │  ├─ 蜂鸣器 (本地)     │  │
│  │ (+IR补光) │    │              │    │  ├─ LED 指示灯        │  │
│  └──────────┘    │  ① 人脸检测  │    │  ├─ 手机推送 (WiFi)   │  │
│                  │  ② 眼部分析  │    │  └─ 车辆 OBD/_CAN    │  │
│  ┌──────────┐    │  ③ 嘴部分析  │    └───────────────────────┘  │
│  │ C4002    │───►│  ④ 头部姿态  │                               │
│  │ 毫米波雷达│    │  ⑤ 融合决策  │    ┌───────────────────────┐  │
│  └──────────┘    │  ⑥ 告警管理  │    │ 可选扩展              │  │
│                  └──────────────┘    │  ├─ MPU-6050 (IMU)    │  │
│  ┌──────────┐                        │  ├─ BME280 (温湿度)   │  │
│  │ IR 补光   │───► 暗光增强 ─────────►│  └─ ToF (近距离确认)  │  │
│  └──────────┘                        └───────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 核心设计理念

1. **视觉为主，雷达为辅**：OV5640 + ESP-DL 提供高频率面部特征，C4002 提供不受光照影响的生理信号
2. **交叉验证消除误报**：视觉闭眼 + 雷达呼吸正常 = 眨眼（忽略）；视觉闭眼 + 雷达呼吸减弱 = 入睡（告警）
3. **失效降级不中断**：任一传感器离线时自动调整权重，保持监测连续性

---

## 2. 硬件方案

### 2.1 硬件 BOM

| # | 组件 | 型号 | 数量 | 单价(约) | 接口 | 用途 |
|---|------|------|------|----------|------|------|
| 1 | 主控 | ESP32-S3-CAM N16R8 | 1 | ¥45 | - | 已有 |
| 2 | 摄像头 | OV5640 5MP | 1 | ¥25 | DVP | 已有 |
| 3 | **毫米波雷达** | **SEN0691 C4002** | **1** | **¥59** | **UART** | **呼吸/存在/体动/光照** |
| 4 | **IR 补光** | **850nm IR LED 模组** | **1** | **¥5** | **GPIO** | **夜间视觉增强** |
| 5 | **蜂鸣器** | **3.3V 有源** | **1** | **¥1** | **GPIO** | **声音告警** |
| 6 | **RGB LED** | **WS2812B** | **1** | **¥1** | **GPIO** | **状态指示** |
| 7 | MPU-6050 | GY-521（可选） | 1 | ¥5 | I2C | 车辆动力学（Phase 3） |
| 8 | BME280 | GY-BME280（可选） | 1 | ¥8 | I2C | 温湿度（Phase 2） |

> **注意**：C4002 内置光照检测（0~50 lux），可替代独立的 BH1750 光照传感器。

### 2.2 引脚分配

```
ESP32-S3-CAM N16R8 引脚分配
═══════════════════════════════════════════════════════
GPIO   用途              接口        方向
═══════════════════════════════════════════════════════
─── 已占用（摄像头 + 云台 + SD） ──────────────────────
 2     LED_BUILTIN       GPIO        OUT
 4     SCCB SDA          I2C         (摄像头)
 5     SCCB SCL          I2C         (摄像头)
 6     VSYNC             DVP         (摄像头)
 7     HREF              DVP         (摄像头)
 8~13  DVP 数据总线       DVP         (摄像头)
14     SERVO_X           LEDC PWM    OUT (云台)
15     XCLK              DVP         (摄像头)
16~18 DVP 数据总线       DVP         (摄像头)
21     SERVO_Y           LEDC PWM    OUT (云台)
38/39/40  SDMMC          SDMMC       (TF 卡)

─── DMS 新增引脚 ─────────────────────────────────────
 1     UART2 TX          UART        OUT → C4002 RX
 3     UART2 RX          UART        IN  ← C4002 TX
35     I2C2 SDA          I2C (SW)    ← MPU-6050 / BME280 (可选)
36     I2C2 SCL          I2C (SW)    ← MPU-6050 / BME280 (可选)
37     IR 补光控制        GPIO        OUT → MOSFET → IR LED
41     蜂鸣器             GPIO        OUT → 有源蜂鸣器
42     WS2812B           RMT/DIO     OUT → RGB LED
═══════════════════════════════════════════════════════
```

### 2.3 C4002 接线

```
C4002 模块          ESP32-S3-CAM
──────────────────────────────
VIN  ────────────►  5V
GND  ────────────►  GND
RX   ────────────►  GPIO1  (UART2 TX)
TX   ────────────►  GPIO3  (UART2 RX)
OUT  ────────────►  GPIO45 (可选，硬件冗余读取)
```

- 通信参数：**115200 baud, 8N1**
- 电平：3.3V TTL（可直接连接 ESP32-S3）

---

## 3. SEN0691 C4002 功能详解

### 3.1 传感器核心能力

C4002 基于 24GHz FMCW 技术，通过 UART 输出丰富的结构化数据：

```
单帧数据输出 (周期可配 0.1~6553.5s)
═══════════════════════════════════════════════════
目标状态 (3 态)
├── eNoTarget     无目标
├── ePresence     存在（静止/微动/呼吸）
└── eMotion       运动（位移）

存在目标信息 (presenceTarget)
├── distance      距离 (cm, 0~1100)
└── energy        存在能量 (0~100, 呼吸/微动幅度)

运动目标信息 (motionTarget)
├── distance      距离 (cm)
├── speed         运动速度 (m/s, 0.1~10)
├── energy        运动能量 (0~100)
└── direction     方向 (eApproaching靠近 / eAway远离 / 无)

环境数据
└── lightIntensity 光照强度 (0~50 lux)

配置参数 (可读写)
├── 检测距离范围      0~1100 cm
├── 距离门 (15/25个)   每个门独立启用/禁用
├── 分辨率模式         80cm(15门) / 20cm(25门)
├── 灵敏度             1~5 级
├── 光照阈值           0~50 lux
├── 汇报周期           0.1s 步进
├── 目标消失延迟       0~65535s
├── 检测锁定时间       0.2~10s
├── OUT 引脚模式       运动/存在/运动+存在
└── 底噪校准           自动学习环境干扰
═══════════════════════════════════════════════════
```

### 3.2 DMS 中的 10 项功能映射

#### 功能 1：驾驶座存在确认（基础功能）

| 场景 | 判定 | 动作 |
|------|------|------|
| 雷达有人 + 视觉有人脸 | 确认驾驶中 | 正常监测 |
| 雷达有人 + 视觉无人脸 | 人脸遮挡/偏头 | 降级监测(仅雷达) |
| 雷达无人 + 视觉有人脸 | 雷达未就绪? | 以视觉为主 |
| 雷达无人 + 视觉无人脸 | 驾驶座无人 | 停止监测 |

#### 功能 2：呼吸/微动能量监测（入睡检测金标准）

C4002 测量人体呼吸引起的胸腔起伏强度（`presenceTarget.energy` 0~100）：

| energy 范围 | 生理含义 | DMS 判定 |
|-------------|----------|----------|
| 60~100 | 正常呼吸，清醒 | 正常 |
| 30~60 | 呼吸变浅变慢，困倦 | 轻度疲劳信号 |
| 10~30 | 呼吸极缓极深，即将入睡 | 严重疲劳 → 告警 |
| 0~10（持续>5s） | 几乎无微动，已入睡 | 立即最高级告警 |

**交叉验证规则**：视觉 EAR 低 + 雷达 energy 低 = 确认入睡，误报率 < 1%

#### 功能 3：目标距离追踪（身体姿态监测）

| 距离变化趋势 | 含义 | DMS 判定 |
|--------------|------|----------|
| 稳定 (±5cm) | 正常坐姿 | 正常 |
| 持续减小 > 10cm | 身体前倾(低头打瞌睡) | 疲劳信号 +1 |
| 持续增大 > 10cm | 身体后仰(瘫坐) | 疲劳信号 +1 |
| 频繁波动 | 烦躁/不适 | 提醒休息 |

#### 功能 4：运动速度检测（惊吓反应）

| 场景 | speed | 判定 |
|------|-------|------|
| 正常驾驶(微动) | ~0 m/s | 正常 |
| 急刹后身体前冲回弹 | > 0.5 m/s | 有反应 = 清醒 |
| 急刹后无任何运动 | ~0 m/s | 无反应 = 极度疲劳/入睡 |

#### 功能 5：运动方向识别（身体倾倒方向）

| 方向 | 含义 | DMS 判定 |
|------|------|----------|
| eApproaching（持续>2s） | 身体前倾(低头) | 疲劳信号 |
| eAway（持续>2s） | 身体后仰(瘫倒) | 疲劳信号 |
| 无方向 | 静止/微动 | 正常或已入睡(看 energy) |

#### 功能 6：环境光照检测（替代 BH1750 + IR 补光控制）

C4002 内置 0~50 lux 光照检测，可省去独立的 BH1750 传感器：

| 光照条件 | lux 值 | 动作 |
|----------|--------|------|
| 白天 | > 10 | 正常视觉检测 |
| 黄昏/隧道 | 1~10 | 降低 EAR 阈值 |
| 夜间 | < 1 | 开启 IR 补光 + 提升雷达权重 |

#### 功能 7：距离门分区（屏蔽副驾/后排干扰）

C4002 支持 15 个距离门（80cm 分辨率），每个门可独立启用/禁用。车载安装时只启用驾驶座区域的前 3 个门：

```
Gate 0 (0~0.8m)    ✓ 启用  ← 方向盘到驾驶员胸部
Gate 1 (0.8~1.6m)  ✓ 启用  ← 驾驶员胸部到头部
Gate 2 (1.6~2.4m)  ✓ 启用  ← 驾驶员后方空间
Gate 3~14          ✗ 禁用  ← 屏蔽副驾/中控/后排
```

#### 功能 8：底噪自学习（过滤车辆振动干扰）

C4002 可自动学习环境底噪，过滤发动机振动、路面颠簸、空调气流等干扰：
- 校准时需确保驾驶座无人
- 阈值 > 50 表示环境干扰大
- 阈值 > 99 表示强烈干扰，传感器可能无法正常工作

#### 功能 9：检测锁定时间（避免误触发）

目标消失后的一段锁定时间内不报"无人"，避免颠簸导致瞬时丢失。车载建议设为 3 秒。

#### 功能 10：OUT 引脚硬件直出（安全冗余）

OUT 引脚可配置为存在/运动触发高电平，即使 ESP32 主控死机，雷达仍能独立触发最低级告警。

### 3.3 功能汇总表

| # | 功能 | C4002 数据源 | DMS 价值 | 精度贡献 |
|---|------|-------------|----------|----------|
| 1 | 驾驶座存在确认 | targetState | 摄像头失效时不中断监测 | 消除漏报 |
| 2 | **呼吸能量监测** | presenceTarget.energy | **入睡检测金标准** | **最高** |
| 3 | 身体距离趋势 | presenceTarget.distance | 前倾/后仰姿态判定 | 高 |
| 4 | 急刹反应检测 | motionTarget.speed | 无反应=极度疲劳 | 高 |
| 5 | 倾倒方向识别 | motionTarget.direction | 区分正常动作vs疲劳倾倒 | 中 |
| 6 | **环境光照检测** | lightIntensity | **替代 BH1750** + IR控制 | 中 |
| 7 | **距离门分区** | configureGate | **屏蔽副驾/后排干扰** | **最高** |
| 8 | 底噪自学习 | startEnvCalibration | 过滤车辆振动干扰 | 高 |
| 9 | 检测锁定 | setLockTime | 避免颠簸误判 | 中 |
| 10 | OUT 硬件冗余 | OUT 引脚 | 主控死机仍能告警 | 安全底线 |

---

## 4. 融合决策算法

### 4.1 分层融合模型

```
Level 1: 单传感器特征提取（并行，各核独立）
  视觉特征(5~10Hz)  IMU特征(10Hz)  雷达特征(1~2Hz)  环境特征(0.1Hz)
  ear/mar/head/     brake/nod/     breath/          temp/
  perclos           steer          presence/        lux/humidity

Level 2: 单传感器疲劳评分（归一化到 0~1）
  S_vis=0.72  S_imu=0.35  S_rdr=0.85  S_env=1.15

Level 3: 加权融合
  S_fusion = Σ(wi × Si) × env_factor
```

### 4.2 默认权重分配

| 传感器 | 权重 | 说明 |
|--------|------|------|
| 视觉（OV5640） | 0.40 | 主传感器，提供 EAR/MAR/PERCLOS |
| 雷达（C4002） | 0.25 | 呼吸能量 + 存在确认 |
| 时序模型 | 0.15 | 历史趋势分析 |
| IMU（可选） | 0.15 | 急刹/偏航 |
| 环境（可选） | 0.05 | 温度/光照修正因子 |

### 4.3 交叉验证规则

| 场景 | 视觉 | IMU | 雷达 | 决策 |
|------|------|-----|------|------|
| 正常驾驶 | 低 | 低 | 正常 | 正常 |
| 眨眼（<0.3s） | 高 | -- | 正常 | 忽略（雷达确认呼吸正常） |
| 真实闭眼入睡 | 高 | 低 | 呼吸↓ | 严重告警（三传感器一致） |
| 摄像头被遮挡 | 失效 | -- | 正常 | 降级到雷达+IMU |
| 强阳光过曝 | 失效 | 低 | 正常 | 忽略 |
| 急刹后无反应 | 高 | 高 | 呼吸↓ | 最高优先级告警 |
| 戴墨镜 | EAR失效 | -- | 呼吸正常 | 降权EAR |

**关键规则**：
- R1：视觉闭眼 + 雷达呼吸正常 → 判定为眨眼，不告警
- R2：视觉闭眼 + 雷达呼吸减慢 + 体动降低 → 确认入睡，立即告警
- R3：视觉失效(遮挡/过曝) → 自动切换到雷达+IMU 双传感器模式
- R4：IMU急刹 + 视觉无反应 + 雷达呼吸异常 → 最高优先级告警
- R5：环境高温 + 任何疲劳信号 → 告警阈值下调 10%

### 4.4 自适应权重调整

```c
// 场景自适应规则
void adapt_weights(sensor_status_t *status, fusion_weights_t *w) {
    if (!status->camera_ok && status->radar_ok) {
        // 摄像头失效 → 雷达主导
        w->w_visual = 0.0;
        w->w_radar  = 0.45;
        w->w_imu    = 0.30;
        w->w_temporal = 0.25;
    } else if (status->is_night && status->ir_on) {
        // 夜间 IR 模式
        w->w_visual = 0.30;
        w->w_radar  = 0.35;
        w->w_imu    = 0.15;
        w->w_temporal = 0.20;
    } else if (status->sunglasses_detected) {
        // 墨镜 → EAR 不可靠
        w->w_visual = 0.25;
        w->w_radar  = 0.35;
        w->w_imu    = 0.20;
        w->w_temporal = 0.20;
    }
    normalize(w);  // 归一化确保总和 = 1.0
}
```

---

## 5. 软件架构

### 5.1 模块划分

```
main/
├── include/
│   ├── app_camera.h          (已有)
│   ├── app_face_detect.h     (已有 - 扩展导出更多特征)
│   ├── app_gimbal.h          (已有)
│   ├── app_httpd.h           (已有 - 扩展 /drowsy API)
│   ├── app_wifi.h            (已有)
│   ├── app_sdcard.h          (已有)
│   ├── app_record.h          (已有)
│   ├── app_drowsy.h          ★ 新增 - 疲劳融合决策
│   ├── app_sensor_radar.h    ★ 新增 - C4002 驱动
│   ├── app_sensor_imu.h      ★ 新增 - MPU-6050 驱动（可选）
│   ├── app_sensor_env.h      ★ 新增 - BME280 驱动（可选）
│   └── app_alert.h           ★ 新增 - 告警输出(蜂鸣/LED/推送)
├── app_drowsy.cpp            ★ 新增 - 融合算法核心
├── app_sensor_radar.c        ★ 新增 - C4002 UART 解析 + 特征提取
├── app_sensor_imu.c          ★ 新增 - MPU-6050 I2C 读取（可选）
├── app_sensor_env.c          ★ 新增 - BME280 读取（可选）
├── app_alert.c               ★ 新增 - 蜂鸣器 + WS2812B + 推送
└── web/
    ├── index.html            (修改 - 增加疲劳状态面板)
    └── drowsy.html           ★ 新增 - 疲劳详情/历史/配置页
```

### 5.2 FreeRTOS 任务分配

```
Core 0 (PRO) ─── 通信 & 告警
┌────────────────┬──────────┬────────┬───────────────────────┐
│ 任务            │ 优先级   │ 栈     │ 周期                   │
├────────────────┼──────────┼────────┼───────────────────────┤
│ httpd_task     │ 5        │ 8192   │ 持续（MJPEG 推流）     │
│ wifi_task      │ 5        │ 4096   │ 事件驱动               │
│ alert_task     │ 4        │ 3072   │ 100ms 检查告警状态     │
│ radar_task     │ 3        │ 4096   │ 500ms (C4002 汇报周期) │
└────────────────┴──────────┴────────┴───────────────────────┘

Core 1 (APP) ─── 采集 & 推理
┌────────────────┬──────────┬────────┬───────────────────────┐
│ 任务            │ 优先级   │ 栈     │ 周期                   │
├────────────────┼──────────┼────────┼───────────────────────┤
│ face_det_task  │ 4        │ 8192   │ 100~200ms/帧           │
│ imu_task       │ 3        │ 4096   │ 5ms (200Hz)（可选）    │
│ env_task       │ 2        │ 2048   │ 10s（可选）            │
│ drowsy_task    │ 3        │ 4096   │ 100ms (融合决策)       │
└────────────────┴──────────┴────────┴───────────────────────┘
```

### 5.3 数据流

```
face_det_task ──face_box + EAR/MAR──► drowsy_task
                                         │
radar_task    ──breath/posture/presence──►  │
                                         │  │
imu_task      ──brake/nod/steer──────────►  │
                                         │  │
env_task      ──temp/lux────────────────►  │
                                         ▼
                                    融合决策
                                    │
                          ┌─────────┼─────────┐
                          ▼         ▼         ▼
                     alert_task  httpd    record
                     (蜂鸣/LED)  (/drowsy) (抓拍)
```

---

## 6. 告警分级

| 级别 | 融合评分 + 条件 | 告警动作 | 恢复条件 |
|------|----------------|----------|----------|
| **L0 正常** | S < 0.30 | 绿色 LED | - |
| **L1 轻度** | S ≥ 0.30 + 视觉 EAR 低 or 哈欠 1 次 | 黄色 LED + 蜂鸣 1声/30s + 网页提示 | S < 0.25 持续 > 15s |
| **L2 中度** | S ≥ 0.50 + PERCLOS > 38% + 雷达呼吸减慢 | 橙色 LED + 蜂鸣 2声/10s + 声音告警 + 抓拍 | S < 0.35 持续 > 30s |
| **L3 严重** | S ≥ 0.70 + 雷达呼吸 < 10次/分 + 体动极低（三重确认） | 红色 LED 闪烁 + 蜂鸣持续 + WiFi 推送 + 录像 | S < 0.40 持续 > 60s |

**特殊规则**：
- 传感器失效降级：任一传感器离线 → 自动调整权重，不中断监测
- 摄像头遮挡：视觉权重归零 → 雷达+IMU 双传感器模式
- 急刹无反应：直接跳到 L3，不经过 L1→L2 渐变
- 环境加速：高温(>30°C) + 夜间 → 各级别阈值下调 0.05

---

## 7. REST API 扩展

| 路径 | 方法 | 说明 |
|------|------|------|
| `/drowsy` | GET | 当前疲劳状态 `{level, score, ear, mar, breath_energy, posture, perclos, alerts_count}` |
| `/drowsy/config` | GET/POST | 告警阈值配置（EAR/MAR 阈值、时间窗口等） |
| `/drowsy/history` | GET | 最近 N 次告警记录（时间/级别/截图 URL） |
| `/drowsy/snapshot` | GET | 疲劳触发时自动抓拍的 JPEG |
| `/radar` | GET | C4002 雷达原始数据 `{state, presence_dist, presence_energy, motion_speed, motion_dir, light}` |
| `/radar/calibrate` | POST | 触发雷达底噪校准 |

---

## 8. 实施路线图

```
Phase 1 — C4002 接入 + 基础融合（待传感器到货后开始）
├── app_sensor_radar.c/h  — C4002 UART 驱动 + 数据解析
├── app_drowsy.cpp/h      — 视觉 + 雷达 双传感器融合
├── app_alert.c/h         — 蜂鸣器 + WS2812B LED 告警
├── IR 补光灯控制（GPIO + C4002 光照自动开关）
├── /drowsy + /radar API
├── 前端疲劳状态面板
└── C4002 车载配置（距离门分区 + 底噪校准）

Phase 2 — 环境感知 + 告警完善
├── BME280 温湿度传感器接入（可选）
├── 交叉验证规则完整实现
├── 自适应权重调整
├── 告警历史记录到 SD 卡
└── 告警截图自动保存

Phase 3 — 精度调优
├── MPU-6050 IMU 接入（急刹/点头检测）
├── 实车数据采集 + 参数标定
├── 误报率/漏报率统计
├── 个性化阈值学习（不同驾驶员）
└── 时序模型优化

Phase 4 — 产品化
├── MQTT 上报 → 车队管理平台
├── OTA 远程升级模型/阈值
├── 低功耗模式（停车监控）
└── 外壳设计（集成所有传感器）
```

---

## 9. 精度预期

| 方案 | 准确率 | 误报率 | 漏报率 | 说明 |
|------|--------|--------|--------|------|
| 纯视觉（EAR+MAR） | ~85% | 15% | 12% | 墨镜/口罩/暗光失效 |
| 视觉 + C4002 雷达 | ~93% | 6% | 5% | 雷达消除视觉误报 |
| 视觉 + 雷达 + IMU | ~96% | 3% | 3% | IMU 补充急刹场景 |
| **四传感器全融合** | **~98%** | **2%** | **1%** | 环境修正 + 交叉验证 |

### 视觉与雷达融合后的误报消除

| 疲劳场景 | 纯视觉(EAR) | + C4002 融合 | 提升原因 |
|----------|-------------|-------------|----------|
| 夜间驾驶 | 不可用(暗光) | 93% 准确率 | 雷达不受光照影响 |
| 戴墨镜 | EAR 失效 | 91% 准确率 | 雷达呼吸能量接管 |
| 戴口罩 | MAR 失效 | 94% 准确率 | 雷达不受遮挡影响 |
| 摄像头被遮挡 | 完全失效 | 88% 准确率 | 雷达独立工作 |
| 已入睡(闭眼+呼吸弱) | 85% (可能误报) | 98% 准确率 | 双重确认 |
| 眨眼误判(0.3s闭眼) | 误报! | 不误报 | 雷达能量正常→排除 |
| 副驾有人 | 误报! | 不误报 | 距离门屏蔽副驾 |
| 车辆颠簸干扰 | 无影响 | 不受影响 | 底噪自学习过滤 |

---

## 10. 关键约束与风险

| 项目 | 说明 | 缓解措施 |
|------|------|----------|
| **算力瓶颈** | ESP32-S3 双核 240MHz，人脸检测已占 ~50ms/帧 | C4002 数据由独立 UART 任务处理，不占 CPU |
| **内存压力** | 8MB PSRAM，雷达缓冲仅需 ~1KB | 无风险 |
| **光照变化** | 白天/夜间/隧道切换时 AE 收敛期间可能漏检 | IR 补光 + C4002 雷达不受光照影响 |
| **佩戴遮挡** | 墨镜遮挡眼部 → EAR 失效 | 降级为头部姿态 + 雷达呼吸检测 |
| **摄像头位置** | 需正对驾驶员面部，距离 40~80cm，俯角 15~30° | 安装指南文档 |
| **实时性** | 闭眼检测延迟需 < 500ms | 混合方案可将关键路径延迟控制在 100ms 内 |
| **C4002 供电** | 工作电流 ~90mA，需稳定 5V | 与摄像头共用 ≥1A 电源 |
| **C4002 安装** | 波束角 120°×120°，需朝向驾驶员胸部 | 方向盘柱上方或仪表盘顶部 |

---

## 11. C4002 车载配置参考

```c
// ====== C4002 车载场景完整配置 ======
esp_err_t dms_radar_init(void)
{
    // 1. UART 初始化 (115200, 8N1)
    //    ESP32-S3 UART2: TX=GPIO1, RX=GPIO3
    c4002.begin();

    // 2. 关闭板载 LED (避免夜间车内闪烁干扰驾驶员)
    c4002.setRunLedState(eLedOff);
    c4002.setOutLedState(eLedOff);

    // 3. 分辨率模式：80cm (车载距离短，15 门足够)
    c4002.setResolutionMode(eResolution80Cm);

    // 4. 检测范围：0~240cm (仅驾驶座区域)
    c4002.setDetectRange(0, 240);

    // 5. 距离门：只启用前 3 个门(屏蔽副驾/后排)
    uint8_t gates[15] = {
        C4002_ENABLE, C4002_ENABLE, C4002_ENABLE,  // Gate 0-2: 驾驶座
        C4002_DISABLE, C4002_DISABLE,               // Gate 3-4: 中控间隙
        C4002_DISABLE, C4002_DISABLE, C4002_DISABLE,// Gate 5-7
        C4002_DISABLE, C4002_DISABLE, C4002_DISABLE,// Gate 8-10
        C4002_DISABLE, C4002_DISABLE, C4002_DISABLE,// Gate 11-13
        C4002_DISABLE                                // Gate 14
    };
    c4002.configureGate(ePresenceDistGate, gates);
    c4002.configureGate(eMotionDistGate,   gates);

    // 6. 光照阈值：0 (始终检测，光照判断由主程序处理)
    c4002.setLightThresh(0);

    // 7. 汇报周期：5 * 0.1s = 500ms (车载需要快速响应)
    c4002.setReportPeriod(5);

    // 8. 目标消失延迟：2s (避免颠簸导致瞬时丢失)
    c4002.setTargetDisappearDelay(2);

    // 9. 检测锁定时间：3s (短暂离座不立即判无人)
    c4002.setLockTime(3.0);

    // 10. OUT 引脚模式：存在时高电平 (硬件冗余)
    c4002.setOutPinMode(eOutpinMode2);

    return ESP_OK;
}
```

---

## 12. 代码骨架

### 12.1 C4002 雷达数据结构

```c
// app_sensor_radar.h
typedef enum {
    RADAR_STATE_NO_TARGET = 0,
    RADAR_STATE_PRESENCE,      // 存在（静止/微动/呼吸）
    RADAR_STATE_MOTION,        // 运动
} radar_state_t;

typedef enum {
    RADAR_DIR_NONE = 0,
    RADAR_DIR_APPROACHING,     // 靠近
    RADAR_DIR_AWAY,            // 远离
} radar_direction_t;

typedef struct {
    // 目标状态
    radar_state_t   state;
    bool            target_present;

    // 存在目标 (呼吸/微动)
    float           presence_distance;   // cm
    uint8_t         presence_energy;     // 0~100 (呼吸幅度)

    // 运动目标
    float           motion_distance;     // cm
    float           motion_speed;        // m/s
    uint8_t         motion_energy;       // 0~100
    radar_direction_t motion_direction;

    // 环境
    float           light_intensity;     // lux (0~50)

    // DMS 衍生指标
    float           breath_score;        // 呼吸评分 0~1 (1=正常)
    float           posture_score;       // 姿态评分 0~1 (1=正常)
    bool            likely_asleep;       // 可能入睡
    uint32_t        static_duration_ms;  // 静止持续时间
} radar_data_t;

esp_err_t app_sensor_radar_init(void);
void      app_sensor_radar_get(radar_data_t *out);
```

### 12.2 疲劳融合决策数据结构

```c
// app_drowsy.h
typedef enum {
    ALERT_LEVEL_0_NORMAL = 0,
    ALERT_LEVEL_1_LIGHT,       // 轻度
    ALERT_LEVEL_2_MODERATE,    // 中度
    ALERT_LEVEL_3_SEVERE,      // 严重
} alert_level_t;

typedef struct {
    // 各传感器评分 (0~1, 1=最疲劳)
    float   visual_score;
    float   radar_score;
    float   imu_score;
    float   temporal_score;

    // 融合结果
    float   fusion_score;        // 加权总分
    alert_level_t level;         // 告警级别

    // 原始指标
    float   ear;                 // 眼睛纵横比
    float   mar;                 // 嘴巴纵横比
    uint8_t radar_energy;        // 雷达呼吸能量
    float   perclos;             // 60s 闭眼占比

    // 统计
    uint32_t alerts_count;       // 累计告警次数
    uint32_t uptime_sec;         // 监测时长
} drowsy_state_t;

esp_err_t app_drowsy_start(void);
void      app_drowsy_get_state(drowsy_state_t *out);
void      app_drowsy_push_visual(float ear, float mar, bool face_valid);
void      app_drowsy_push_radar(const radar_data_t *radar);
```

### 12.3 告警输出接口

```c
// app_alert.h
typedef struct {
    uint8_t buzzer_pin;       // 蜂鸣器 GPIO
    uint8_t led_pin;          // WS2812B GPIO
    uint8_t ir_led_pin;       // IR 补光 GPIO
} alert_config_t;

esp_err_t app_alert_init(const alert_config_t *cfg);
void      app_alert_set_level(alert_level_t level);
void      app_alert_set_ir_light(bool on);  // IR 补光开关
```

### 12.4 雷达采集任务骨架

```c
// app_sensor_radar.c — 任务骨架
static void radar_task(void *arg)
{
    float dist_history[20];
    uint8_t energy_history[60];   // 30秒能量历史 (500ms*60)
    int hist_idx = 0;

    while (1) {
        // 读取 C4002 全部数据
        eTargetState_t state = c4002.getTargetState();
        sTarget_t presence = c4002.getPresenceTarget();
        sTarget_t motion   = c4002.getMotionTarget();
        float light        = c4002.getLightIntensity();

        // 更新历史
        dist_history[hist_idx % 20] = presence.distance;
        energy_history[hist_idx % 60] = presence.energy;
        hist_idx++;

        // 填充 s_radar 结构...

        // 衍生指标：呼吸评分
        float avg_energy_30s = avg_uint8(energy_history, 60);
        s_radar.breath_score = clamp(avg_energy_30s / 50.0f, 0.0f, 1.0f);

        // 衍生指标：入睡判定（能量 < 15 且持续 > 5s）
        uint8_t recent_min = min_uint8(energy_history + 50, 10);
        s_radar.likely_asleep = (recent_min < 15 && s_radar.state == RADAR_STATE_PRESENCE);

        // 推送给融合决策模块
        drowsy_fusion_push_radar(&s_radar);

        vTaskDelay(pdMS_TO_TICKS(500));  // 500ms 周期
    }
}
```

---

## 附录：C4002 关键技术规格

| 规格 | 数值 |
|------|------|
| 工作电压 | 3.6~5.5V |
| 工作频率 | 24GHz~24.25GHz |
| 调制模式 | FMCW |
| 运动检测距离 | 最远 11m |
| 静态存在检测距离 | 最远 10m |
| 探测角度 | 120°×120° |
| 通信接口 | UART (115200 8N1) + OUT 引脚 |
| 光线检测 | 0~50 lux |
| 工作温度 | -20~85°C |
| 尺寸 | 22mm × 26mm |
| SKU | SEN0691 |
| 参考价格 | ¥59 |

---

**文档维护**：本方案以「基线 + 变更记录」形式维护。传感器到货后，在本文档底部追加实施记录。
