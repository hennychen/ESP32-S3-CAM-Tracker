# 闭眼检测分层降级方案（④+③）

> ESP32-S3-CAM-Tracker 项目的闭眼检测架构设计文档。
> 主线：方案④（上位机 MediaPipe 468点 EAR）；兜底：方案③（ESP32 端轻量 CNN）。

---

## 一、总体架构

```
┌──────────────────────────────────────────────────────────────┐
│                    ESP32-S3 (数据采集终端)                     │
│                                                              │
│  摄像头 → JPEG → /capture 推流                                │
│         → 5点检测 → /face JSON                               │
│         → 方案③ 离线CNN (兜底，仅在上位机离线时激活)           │
│                                                              │
│  降级决策器：                                                  │
│    /heartbeat 请求被上位机访问 → 在线模式 (④优先)              │
│    超过 15秒 无心跳          → 离线模式 (③接管)               │
└──────────────────┬───────────────────────────────────────────┘
                   │ WiFi
                   ▼
┌──────────────────────────────────────────────────────────────┐
│                 上位机 (手机/电脑浏览器)                        │
│                                                              │
│  方案④ 主线：MediaPipe 468点 → EAR → 闭眼检测                 │
│  心跳上报：每 5秒 GET /heartbeat → 维持在线状态                │
│  闭眼告警：推送指令到 ESP32 (蜂鸣/录像/LED)                    │
└──────────────────────────────────────────────────────────────┘
```

### 当前已完成的基础

| 组件 | 状态 | 说明 |
|------|------|------|
| `/capture` JPEG 推流 | ✅ 已完成 | img.src 自驱动轮询 ~5fps |
| `/face` 5点关键点 JSON | ✅ 已完成 | 含 box/kp/roll/vr/drowsy |
| MediaPipe 468点 EAR | ✅ 已完成 | 按需加载，GPU加速，前端运行 |
| 前端 EAR 显示 | ✅ 已完成 | OPEN/CLOSED 实时显示 |
| 5点关键点可视化 | ✅ 已完成 | 青色眼/黄色鼻/橙色嘴 |
| 粗略疲劳检测(姿态) | ✅ 已完成 | roll + vert_ratio → drowsy |

---

## 二、方案 ④ 主线增强（4项）

### 4-1. 闭眼持续时间判断

标准疲劳检测要求"连续闭眼 > 1秒"才算微睡眠事件。

```
前端 JS：
  closedFrames 计数器
  EAR < 0.20 → closedFrames++
  EAR >= 0.20 → closedFrames = 0
  closedFrames > N (N = fps × 秒数) → 触发告警

告警分级：
  闭眼 0.5~1s  → 黄色提醒 "请注意"
  闭眼 1~3s   → 橙色警告 "疲劳驾驶"
  闭眼 >3s    → 红色警报 "危险！唤醒"
```

### 4-2. EAR 滑动平均滤波

原始 EAR 有帧间抖动，用 5 帧滑动窗口平滑：

```javascript
const earHistory = [];
const EAR_WINDOW = 5;
function smoothEAR(rawEar) {
  earHistory.push(rawEar);
  if (earHistory.length > EAR_WINDOW) earHistory.shift();
  return earHistory.reduce((a,b) => a+b) / earHistory.length;
}
```

### 4-3. 心跳机制（降级触发器）

```
前端每 5 秒发一次 GET /heartbeat?t=...
ESP32 端记录 last_heartbeat_ms

降级逻辑（ESP32 端）：
  now - last_heartbeat_ms < 15000  → 在线模式，方案④主导
  now - last_heartbeat_ms >= 15000 → 离线模式，方案③接管
```

### 4-4. 上位机告警指令下发

前端检测到闭眼事件后，向 ESP32 发送指令：

```
GET /alert?level=warning   → ESP32 蜂鸣短响 + LED黄
GET /alert?level=danger    → ESP32 蜂鸣长响 + LED红 + 触发录像
GET /alert?level=clear     → 清除告警状态
```

---

## 三、方案 ③ 离线兜底（全新实现）

### 3.1 模型选择：ClosedEye-Net

极轻量专为嵌入式设计的闭眼分类 CNN：

```
输入: 32×24 灰度 (眼部 ROI)
Conv2D(8, 3×3) + ReLU + MaxPool(2×2)     → 15×11×8
Conv2D(16, 3×3) + ReLU + MaxPool(2×2)    → 6×4×16
Conv2D(32, 3×3) + ReLU + MaxPool(2×2)    → 2×1×32
Flatten → Dense(2, Softmax)
输出: [open_prob, closed_prob]
```

| 参数 | 值 |
|------|-----|
| 输入尺寸 | 32×24×1 (灰度) |
| 参数量 | ~20K |
| int8 量化后大小 | ~20KB |
| ESP32 推理耗时 | ~5-10ms |

### 3.2 模型获取途径

```
步骤 1: 准备数据集
  ├─ CEW (Closed Eye in the Wild) 数据集：2442 张眼部图像
  │   下载: http://parnec.nuaa.edu.cn/xtan/data/ClosedEyeDatabases.html
  └─ 或自采：用当前 ESP32 拍照收集睁/闭眼样本各 500+

步骤 2: PyTorch 训练
  ├─ 数据增强：随机翻转/亮度/对比度
  ├─ 训练 50 epochs (Adam, lr=1e-3)
  └─ 目标准确率：>90%

步骤 3: 导出 ONNX
  └─ torch.onnx.export(model, dummy_input, "closed_eye.onnx")

步骤 4: esp-ppq 量化为 int8
  ├─ from esp_ppq import quantize_onnx_on_target
  ├─ calibration_dataset = 200 张样本
  └─ output: closed_eye_s8.espdl

步骤 5: 部署
  └─ 拷贝到 managed_components/espressif__human_face_detect/models/s3/closed_eye_s8.espdl
```

### 3.3 ESP32 端推理流程

```cpp
// 在 app_face_detect.cpp 的检测循环中，方案③激活时执行

// 步骤1: 从5点关键点裁剪眼部 ROI
// 左眼中心 kp[0],kp[1]，右眼中心 kp[6],kp[7]
// 人脸框宽 fw = r.w, 高 fh = r.h
int eye_w = r.w * 0.25;   // 眼部ROI宽
int eye_h = r.h * 0.10;   // 眼部ROI高

// 左眼 ROI
int lex = r.kp[0] - eye_w/2, ley = r.kp[1] - eye_h/2;
// 右眼 ROI
int rex = r.kp[6] - eye_w/2, rey = r.kp[7] - eye_h/2;

// 步骤2: 从 RGB565 缓冲裁剪 + 缩放到 32×24 灰度
uint8_t eye_gray[2][32*24];  // [0]=左眼 [1]=右眼
crop_rgb565_to_gray(rgb, img_w, img_h, lex, ley, eye_w, eye_h, eye_gray[0], 32, 24);
crop_rgb565_to_gray(rgb, img_w, img_h, rex, rey, eye_w, eye_h, eye_gray[1], 32, 24);

// 步骤3: CNN 推理
float open_prob[2], closed_prob[2];
eye_classifier->run(eye_gray[0], &open_prob[0], &closed_prob[0]);
eye_classifier->run(eye_gray[1], &open_prob[1], &closed_prob[1]);

// 步骤4: 双眼都闭才算闭眼
float avg_closed = (closed_prob[0] + closed_prob[1]) / 2;
bool eyes_closed = (avg_closed > 0.6);
```

### 3.4 眼部 ROI 裁剪函数

```cpp
/**
 * 从 RGB565 图像中裁剪指定区域并转换为灰度，缩放到目标尺寸
 * @param rgb565   源图像缓冲
 * @param src_w    源图像宽度
 * @param src_h    源图像高度
 * @param crop_x   裁剪起始 x
 * @param crop_y   裁剪起始 y
 * @param crop_w   裁剪宽度
 * @param crop_h   裁剪高度
 * @param out_gray 输出灰度缓冲
 * @param dst_w    目标宽度 (如 32)
 * @param dst_h    目标高度 (如 24)
 */
void crop_rgb565_to_gray(const uint8_t *rgb565, int src_w, int src_h,
                         int crop_x, int crop_y, int crop_w, int crop_h,
                         uint8_t *out_gray, int dst_w, int dst_h)
{
    // 最近邻缩放
    float sx = (float)crop_w / dst_w;
    float sy = (float)crop_h / dst_h;
    for (int dy = 0; dy < dst_h; dy++) {
        for (int dx = 0; dx < dst_w; dx++) {
            int sx_i = crop_x + (int)(dx * sx);
            int sy_i = crop_y + (int)(dy * sy);
            // 边界裁剪
            if (sx_i < 0) sx_i = 0; if (sx_i >= src_w) sx_i = src_w-1;
            if (sy_i < 0) sy_i = 0; if (sy_i >= src_h) sy_i = src_h-1;
            // RGB565 → 灰度 (luma = 0.299R + 0.587G + 0.114B)
            uint16_t px = ((uint16_t*)rgb565)[sy_i * src_w + sx_i];
            uint8_t r = (px >> 11) & 0x1F;
            uint8_t g = (px >> 5) & 0x3F;
            uint8_t b = px & 0x1F;
            out_gray[dy * dst_w + dx] = (r * 77 + g * 150 + b * 29) >> 8;
        }
    }
}
```

---

## 四、降级状态机

```
                    ┌─────────────┐
          上位机上线 │  ONLINE     │ 上位机离线 >15s
           ┌───────│  方案④主导   │─────────────┐
           │        │  MediaPipe   │             │
           │        │  EAR 精确检测 │             │
           │        └─────────────┘             ▼
           │                              ┌─────────────┐
           │                              │  OFFLINE    │
           │                              │  方案③接管   │
           │                              │  CNN 闭眼   │
           │                              │  + 持续计数  │
           │                              └─────────────┘
           │                                    │
           │                              上位机重新上线
           │                                    │
           └────────────────────────────────────┘
```

ESP32 端状态变量：

```cpp
// app_face_detect.cpp
static bool     s_online_mode = false;
static uint32_t s_last_heartbeat_ms = 0;
static uint32_t s_closed_frames = 0;   // 方案③连续闭眼帧数

// 每次检测循环开始时检查
bool check_online() {
    uint32_t now = esp_timer_get_time() / 1000;
    return s_online_mode && (now - s_last_heartbeat_ms < 15000);
}
```

---

## 五、接口定义

### 新增 HTTP API

| 路径 | 方法 | 说明 | 响应 |
|------|------|------|------|
| `/heartbeat` | GET | 上位机心跳（每5秒一次） | `{"ok":true,"mode":"online"}` |
| `/alert` | GET | 上位机下发告警指令 | `?level=warning/danger/clear` |

### `/face` JSON 扩展字段

```json
{
  "valid": true,
  "x": 87, "y": 98, "w": 72, "h": 103,
  "score": 1.00,
  "frame": 225,
  "iw": 320, "ih": 240,
  "kp": [149,140,154,157,156,145,164,134,167,152],
  "roll": -2.0,
  "vr": 0.47,
  "drowsy": false,

  "mode": "offline",
  "eyes_closed": true,
  "closed_frames": 12,
  "closed_seconds": 2.4
}
```

### face_result_t 扩展

```c
typedef struct {
    bool     valid;
    int      x, y, w, h;
    float    score;
    uint32_t frame_id;
    int      img_w, img_h;
    int      kp[10];
    float    roll;
    float    vert_ratio;
    bool     drowsy;
    // 新增字段
    char     mode[8];          // "online" / "offline"
    bool     eyes_closed;      // 方案③闭眼结果
    uint32_t closed_frames;    // 连续闭眼帧数
    float    closed_seconds;   // 连续闭眼秒数
} face_result_t;
```

---

## 六、新增文件和改动范围

```
main/
├── include/
│   ├── app_httpd.h          # face_result_t 扩展 mode/eyes_closed/closed_frames
│   └── app_eye_classify.h   # 新增：眼部CNN分类器接口
├── app_face_detect.cpp      # 降级逻辑 + 眼部裁剪 + CNN调用
├── app_eye_classify.c       # 新增：CNN模型加载 + 推理封装
├── app_httpd.c              # 新增 /heartbeat 和 /alert handler
└── web/index.html           # 增强：心跳 + 持续时间 + EAR平滑 + 告警UI

managed_components/
└── espressif__human_face_detect/models/s3/
    └── closed_eye_s8.espdl  # 新增：闭眼检测模型 (~20KB)
```

---

## 七、实施任务分解

### 阶段一：方案 ④ 增强（纯前端，不烧录固件）

| # | 任务 | 文件 |
|---|------|------|
| 1 | EAR 滑动平均滤波 (5帧窗口) | index.html |
| 2 | 闭眼持续时间计数 + 分级告警 | index.html |
| 3 | 心跳上报 (每5秒 GET /heartbeat) | index.html |
| 4 | 告警指令下发 (GET /alert?level=) | index.html |
| 5 | 告警 UI 增强 (黄/橙/红三级) | index.html |

### 阶段二：ESP32 端心跳 + 降级（需烧录）

| # | 任务 | 文件 |
|---|------|------|
| 1 | /heartbeat handler + last_heartbeat 记录 | app_httpd.c |
| 2 | /alert handler (蜂鸣/LED/录像触发) | app_httpd.c |
| 3 | face_result_t 增加 mode/eyes_closed/closed_frames | app_httpd.h |
| 4 | app_face_detect.cpp 降级状态机 | app_face_detect.cpp |

### 阶段三：方案 ③ CNN 模型（最大工作量）

| # | 任务 | 说明 |
|---|------|------|
| 1 | CEW 数据集下载 + 预处理 | 2442张眼部图像 |
| 2 | PyTorch 训练 ClosedEye-Net | 目标准确率>90% |
| 3 | ONNX 导出 + esp-ppq 量化 | 生成 .espdl |
| 4 | app_eye_classify.c 模型加载+推理 | 封装 ESP-DL 调用 |
| 5 | 眼部 ROI 裁剪函数 crop_rgb565_to_gray | 图像处理 |
| 6 | 集成到 face_detect 任务 | 降级模式时调用 |

### 阶段四：联调测试

| # | 任务 | 说明 |
|---|------|------|
| 1 | 在线模式 EAR 精度验证 | MediaPipe + 持续时间告警 |
| 2 | 离线模式 CNN 精度验证 | 关闭浏览器后 15s 降级 |
| 3 | 降级切换平滑性 | 在线↔离线切换无丢失 |
| 4 | 光照/眼镜鲁棒性 | 多人多种条件测试 |

---

## 八、性能目标

| 指标 | 在线模式 (④) | 离线模式 (③) |
|------|-------------|-------------|
| 闭眼检测精度 | 95-99% | 85-92% |
| 检测延迟 | ~130ms | ~60ms |
| ESP32 fps | ~5-6 | ~4-5 |
| 告警响应时间 | <2s | <2s |
| 模型大小 | 0 (浏览器端) | ~20KB Flash |

---

## 九、四种方案对比（决策依据）

| 维度 | ① 像素分析 | ② 106点模型 | ③ 轻量CNN | ④ 数据采集终端 |
|------|-----------|------------|----------|--------------|
| 检测精度 | ★★☆ 中等 | ★★★ 高 | ★★★ 高 | ★★★★ 最高 |
| ESP32 CPU 占用 | 极低 (~2ms) | 高 (~100ms) | 中等 (~10ms) | 最低 (仅编码) |
| 额外 Flash | 0 | +300KB~1MB | +20KB | 0 |
| 额外 RAM | ~2KB | +50KB | +10KB | 0 |
| fps 影响 | 几乎无 | 降到 ~3fps | 微降 | 无 |
| 独立运行 | ✅ 是 | ✅ 是 | ✅ 是 | ❌ 需上位机 |
| 开发工作量 | 1-2 天 | 2-3 周 | 3-5 天 | 1 天 |
| 光照鲁棒性 | 差 | 好 | 较好 | 最好 |
| 眼镜干扰 | 严重 | 轻微 | 较轻 | 轻微 |
| 多角度适应 | 差（仅正面） | 好 | 中等 | 最好 |

**最终选择：④为主线 + ③为兜底**，兼顾精度（在线时最高）和可靠性（离线时不中断）。

---

## 十、关键资源

| 资源 | 地址 |
|------|------|
| CEW 数据集 | http://parnec.nuaa.edu.cn/xtan/data/ClosedEyeDatabases.html |
| esp-ppq 量化工具 | https://github.com/espressif/esp-dl/tree/master/tools/quantization |
| MediaPipe FaceLandmarker 模型 | https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task |
| MediaPipe JS 库 | https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.18/vision_bundle.mjs |
| EAR 论文 | Soukupova & Cech, 2016 "Real-Time Eye Blink Detection using Facial Landmarks" |
| ESP-DL Issue #261 | https://github.com/espressif/esp-dl/issues/261 |

---

## 十一、ClosedEye-Net 训练参考代码

```python
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader
# 假设已有 EyeDataset 类

class ClosedEyeNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 8, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2, 2),   # 32x24 -> 16x12
            nn.Conv2d(8, 16, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2, 2),  # 16x12 -> 8x6
            nn.Conv2d(16, 32, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2, 2), # 8x6 -> 4x3
        )
        self.classifier = nn.Sequential(
            nn.Flatten(),
            nn.Linear(4*3*32, 2),
        )
    def forward(self, x):
        return self.classifier(self.features(x))

# 训练
model = ClosedEyeNet()
criterion = nn.CrossEntropyLoss()
optimizer = optim.Adam(model.parameters(), lr=1e-3)

for epoch in range(50):
    for images, labels in train_loader:
        out = model(images)
        loss = criterion(out, labels)
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

# 导出 ONNX
dummy = torch.randn(1, 1, 24, 32)
torch.onnx.export(model, dummy, "closed_eye.onnx", input_names=["input"], output_names=["output"])
```

### esp-ppq 量化脚本

```python
from esp_ppq import quantize_onnx_on_target
from esp_ppq.api import espdl_export_platform

quantize_onnx_on_target(
    onnx_import_file="closed_eye.onnx",
    espdl_export_file="closed_eye_s8.espdl",
    target=espdl_export_platform.ESP32S3,
    calibration_dataloader=calib_loader,  # 200张样本
    input_shape=[1, 1, 24, 32],
    input_dtype="int8",
)
```
