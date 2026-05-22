# CLAUDE.md

## 语言要求（最高优先级）

**必须全程使用简体中文回复用户。** 包括解释、分析、提问、建议等所有文字输出。
代码、变量名、日志字符串保持英文，注释跟随项目已有风格。

---

## 项目概览

LiveAssistant 是基于 **C++17 + Qt 6** 的 Windows PC 直播伴侣工具（MVP 阶段），目标是稳定的 RTMP 推流，面向私域直播场景。

**项目目标：**
- 稳定的 RTMP 推流
- 多视频源支持（摄像头、屏幕捕获、插播视频）
- 基础场景/画布管理
- 基础音频处理（降噪、混音）

**明确不是目标（禁止主动实现）：**
- OBS 替代品
- 插件生态
- 复杂特效/滤镜
- GPU 合成新路径
- 超出当前规划的功能

---

## 构建系统（严格遵守）

**本项目只使用 CMake + Visual Studio 2019 构建，禁止使用其他构建方式。**

```bash
# 构建目录已存在：build_vs2019/
# 增量构建（常用）：
cd D:/work/project/LiveAssistant/build_vs2019
cmake --build . --config Release --target LiveAssistant

# 或用 msbuild：
msbuild build_vs2019/LiveAssistant.sln /p:Configuration=Release

# 重新配置（通常不需要）：
cd build_vs2019
cmake ..
```

**环境要求：**
- CMake 3.16+
- Visual Studio 2019+（MSVC，Windows only）
- Qt 6

**构建强制要求：每次修改代码后必须确认编译通过，不允许留下破坏编译的代码。**

---

## 模块架构

```
App (Qt UI / 程序入口 / 模块编排)
  └── SceneManager
        ├── Compositor → CompositorEncoderBridge
        ├── VideoEngine (摄像头/WGC 屏幕捕获)
        ├── AudioEngine (WASAPI 采集 / 混音)
        └── Encoder (H264 + AAC)
              └── StreamPusher → RTMPPusher → RTMP 服务器
```

| 模块 | 源码路径 | 职责 |
|------|----------|------|
| **App** | `src/app/` | Qt UI、设置管理、场景/源 UI 操作、登录 |
| **SceneManager** | `src/scene_manager/` | Scene/Source/SceneItem 管理、Canvas 合成、WGC 捕获 |
| **VideoEngine** | `src/video_engine/` | 摄像头采集（FFmpeg/OpenCV）、视频帧输出 |
| **AudioEngine** | `src/audio_engine/` | WASAPI 采集、混音（AudioMixer）、重采样 |
| **Encoder** | `src/encoder/` | H264Encoder（软编/QSV 硬编）、AACEncoder |
| **StreamPusher** | `src/stream_pusher/` | 推包队列、RTMPPusher（libavformat） |
| **MediaPipeline** | `src/media_pipeline/` | MediaFileSource（插播视频 FFmpeg 解码）、抽象 Source/Output 接口 |
| **HTTP** | `src/http/` | HttpClient、业务 API、DownloadManager |
| **Common** | `src/common/` | Log、MediaClock、ConfigManager、FFmpegRAII 等公共工具 |

**关键设计模式：**
- `Scene / Source / SceneItem`（仿 OBS）：Scene 是容器，Source 是抽象采集源，SceneItem = Source + Transform
- `CompositorEncoderBridge`：推流核心协调器（QObject），持有 Compositor、Encoder、StreamPusher、AudioEngine
- `AppSettings`：单一配置数据源，通过 `ISettingsObserver` / `SettingsApplier` 分发变更

---

## 第三方库

均位于 `third_party/`，禁止替换或引入其他同类库：

| 库 | 用途 |
|----|------|
| **FFmpeg** | 音视频编解码、格式封装、RTMP 推流 |
| **x264** | H.264 软件编码 |
| **Intel VPL** | QSV 硬件编码 |
| **Opus** | 音频编解码 |
| **OpenCV** | 摄像头采集（备用路径） |
| **VLC** | 媒体文件播放（vlc_player） |
| **libcurl** | HTTP 请求（已有封装，不直接用） |
| **QCustomPlot** | 音频波形可视化 |
| **KissFFT** | 音频频谱分析 |

---

## 音视频核心规则

### 时间戳系统

- **唯一时钟源**：`MediaClock::instance().now_us()`（单位微秒，单调递增）
- **帧时间戳类型**：`int64_t timestamp_ms`（毫秒）
- 时间戳在帧捕获时生成一次，后续处理不得修改
- 禁止用浮点数做时间转换

**统一时间基：**
- 编码器 `encoder_time_base = {1, 1000}`（毫秒）
- 视频 PTS：`timestamp_ms - first_frame_timestamp_ms_`（相对毫秒）
- 音频 PTS：内部 `next_pts_` 单调累加，帧时长 = `frame_samples * 1000 / sample_rate`（1024 samples @ 48kHz ≈ 21ms）

### 修改 PTS/时间戳前的强制要求

1. 完全理解现有逻辑
2. 确认音视频 PTS 同步关系
3. 理解涉及的 time_base
4. 向用户说明修改方案，等待确认

**绝对禁止：**
- 随意修改 time_base
- 同时修改音频和视频 PTS 逻辑
- 未经验证的连续修改

### 编码器操作禁令

以下操作**未经用户明确确认，绝对不执行**：

| 禁止操作 | 原因 |
|----------|------|
| 调用编码器 `reset()` | 导致状态混乱 |
| 调用编码器 `flush()` | 可能破坏内部缓冲区 |
| 实现自动 fallback 机制 | 掩盖真实错误 |
| 实现自动 retry 逻辑 | 掩盖真实错误 |

**编码失败时的正确处理：** 查日志 → 分析根因 → 告知用户 → 等待指令

### 推流排查检查清单

1. 编码器初始化是否成功？
2. 有无编码错误日志？
3. FLV 文件是否包含音视频帧？
4. 音视频 PTS 是否同步？
5. RTMP 推流是否成功？

### 音频混音架构

- 混音来源：麦克风（WASAPI）、系统扬声器（loopback）、插播媒体（MediaFileSource）
- 混音模式：MIC_ONLY / SPEAKER_ONLY / MEDIA_ONLY / MIC_SPEAKER / MIC_MEDIA / SPEAKER_MEDIA / MIC_SPEAKER_MEDIA
- `AudioMixer` 实现混音，`AudioEngine` 协调调度

---

## 编码规范

| 规范 | 要求 |
|------|------|
| C++ 标准 | C++17 |
| 单个 .cpp 文件 | ≤ 400 行 |
| 类职责 | 单一职责，一个类对应独立 .h/.cpp 文件对 |
| 指针管理 | `shared_ptr` / `unique_ptr`，禁止共享裸指针 |
| 模板 | 不使用复杂模板 |
| 宏 | 不用宏实现核心逻辑 |
| 注释 | 解释"为什么"，不解释"代码在做什么" |
| 命名 | 类成员 `xxx_`，方法 `snake_case()`，类名 `PascalCase` |
| 错误处理 | MVP 阶段优先日志，不追求复杂恢复机制 |

---

## 决策流程

实现功能时：
1. **先尝试用户要求的方案**，即使困难
2. **不可行时明确告知**，不自行切换 Plan B
3. **等待用户指令**再继续

以下情况**必须先问用户**：
- 修改用户明确要求的实现方式
- 换技术栈或引入新库
- 修改核心架构
- 降低功能或性能要求

---

## 模块边界规则

- UI（App）不直接访问编码器内部
- SceneManager 不知道推流的存在
- Encoder 只处理已准备好的帧，不关心帧来源
- 模块间通过明确接口通信，禁止直接依赖具体实现类

---

## 参考项目

| 项目 | 路径 | 用途 |
|------|------|------|
| OBS Studio | `D:\work\project\obs-studio` | Scene/Source/SceneItem 架构参考 |
| 原项目 | `C:\Users\hxyli\work\code\qt-live-client` | 历史实现参考（InsertFilePlayer 等） |

---

## 关键文档索引

| 文档 | 内容 |
|------|------|
| `docs/AI_CONSTRAINTS.md` | AI 行为约束完整版 |
| `DESIGN_DOC.md` | 系统设计文档 |
| `MEDIA_TIMESTAMP_SPEC.md` | 时间戳规范详细版 |
| `WGC_DESIGN_SCHEME.md` | Windows 屏幕捕获设计 |
| `docs/INSERT_VIDEO_DESIGN.md` | 插播视频功能设计 |
| `docs/streaming_architecture.md` | 推流架构详解 |
