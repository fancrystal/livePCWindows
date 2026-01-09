# Architecture Design

## 模块划分

### 1. App
- 程序入口
- Qt UI
- 模块调度

### 2. SceneManager
- Scene 管理
- Source 管理
- Transform 数据

### 3. VideoEngine
- 视频采集
- 视频合成
- 视频帧输出

### 4. AudioEngine
- 音频采集
- 音频处理
- 音频混音

### 5. Encoder
- 视频编码
- 音频编码

### 6. StreamPusher
- RTMP 推流
- 推流状态管理

## 模块边界
- UI 不直接访问编码器内部
- Scene 不知道推流存在
- Encoder 只处理已经准备好的帧
- 模块间通过明确接口通信

## 禁止事项
- 模块之间直接依赖具体实现
- 共享裸指针
