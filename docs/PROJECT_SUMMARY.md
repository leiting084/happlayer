# happlayer v1.2 — 项目交付总结

> 多层 HAP / H.264 / Animation 视频播放器（C++17 + FFmpeg + GLFW + OpenGL + SDL2 + Vulkan）。
> 单个 `happlayer.exe` 同时驱动 30+ 层视频，按配置自动切场。
> 适用场景：展厅、临展、自助终端、广告机。

---

## 一、项目定位

`happlayer` 是一个**多队列、多 codec、多层透明视频播放器**。每个 `[queue]` 配置是一组从下往上叠加的视频图层，每个图层独立解码线程；非循环图层播完后自动切到下一队列；最后一个队列结束回到第一个循环。

**解决什么问题**：展厅常见的"几十层 HAP 视频按脚本自动播放 + 切换场景"的需求，传统 VLC/ffplay 不支持多图层 + 队列编排 + 自动切场 + 4K 直角拼接大屏 + 开机自启 + UDP 远程控制。

**目标场景**：
- 展厅 / 临展 / 自助终端（24/7 无人值守）
- 4K 直角拼接大屏（Win11 DWM 圆角绕过）
- 远程控制（UDP 端口命令控制）
- 性能敏感（15-30 层 4K HAP 60fps）

**技术栈**：
- C++17（MSVC v143 / MinGW）
- FFmpeg（解封装 + 软解 + NVDEC 硬解）
- GLFW（窗口管理）
- OpenGL 1.5+（主渲染路径，DXT 压缩纹理直传）
- SDL2（音频子系统和 Vulkan 窗口后备）
- Vulkan 1.0（可选加速路径，HAP Q shader + DXT 上传 + 单 surface 管线）
- Snappy（HAP 帧压缩解压）
- Winsock2（UDP 控制端口）

---

## 二、核心功能

### 2.1 多 codec 支持

| Codec | 路径 | CPU 占用 |
|-------|------|---------|
| **HAP**（DXT1） | GPU 直传压缩纹理，零 CPU 解码 | 接近 0 |
| **Hap Alpha**（DXT5） | GPU 直传，含 alpha | 接近 0 |
| **Hap Q**（YCoCg DXT5） | DXT5 直传 + GPU shader 转 RGB | 接近 0 |
| **Animation**（QTRLE） | FFmpeg 软解 + sws_scale → RGBA8 | 中等 |
| **H.264**（YUV420P） | FFmpeg 软解 / NVDEC 硬解 + sws_scale | 软解高 / 硬解极低 |

### 2.2 多队列调度

- 每个 `[queue]` 是独立场景，由配置文件描述
- 层顺序 = 从下往上叠（alpha blend）
- 每个层有 `开始秒|循环(0/1)|视频路径`
- 非循环层全部结束后自动切下一队列
- 全部队列播完回到第一个队列循环
- 循环层永远不退出（队列会卡住，需注意）

### 2.3 4K 直角显示

- `[window] borderless=1` 触发无边框窗口
- `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_DONOTROUND)` 强制 Win11 拼接大屏无圆角
- 多显示器拼接写总尺寸（如 `3840x2160`）

### 2.4 音频播放

- SDL2 AudioStream API（现代回调）
- 双源支持：HAP 自带音频 / 外挂 mp3-wav
- 视频为主时钟，音频 ±50ms 漂移补偿（drop/repeat 静音段）

### 2.5 开机自启三件套

- `scripts/install-autostart.bat`：写注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer`
- `scripts/set-config.bat`：交互式配置 ExePath + ConfigPath
- `scripts/uninstall-autostart.bat`：删除注册表项
- 所有 .bat 遵循 UTF-8 无 BOM + CRLF + `chcp 65001 >nul`（含中文 echo）

### 2.6 杂项便利

- **防休眠**：`SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)` 内置
- **UDP 控制**：默认 7000 端口，支持 `play/pause/toggle/restart/next/prev/queue N/status/quit`
- **混合显卡优先独显**：`NvOptimusEnablement = 0x1` + `AmdPowerXpressRequestHighPerformance = 1`
- **HEVC 禁用**：遵循全局规则，Chromium 内核依赖未内置 + 双重专利池

---

## 三、性能优化历程

### P0.1 PBO 异步纹理上传

- `glGenBuffers` + `glBufferData` + `glMapBufferRange` 实现异步 ring
- `glFenceSync` + `glClientWaitSync` 同步
- **4K 15 层主线程压力：~125ms → ~50-60ms（~2× 加速）**

### P0.2 snappy 并行解压（最大单点收益）

- 主解码线程只读 packet 并 push 到 pktQueue
- snappy worker 线程（默认 2 个）从 pktQueue 取 packet 做 `hap::decodeInto` 再 push 到 ring
- **主线程解码速度：33 → 212 fps（6.4× 提速）**
- 仅 HAP 层启用（QTRLE/H.264 走 FFmpeg 软解，多线程收益小）

### P0.3 shared_mutex 读写分离

- `std::shared_mutex` + `std::shared_lock` 替代 `std::mutex` + `std::unique_lock`
- `decoderRun` 用 `unique_lock` 写入；`updateLayer` 用 `shared_lock` 并发读
- `std::condition_variable_any` 配合 shared_mutex
- **替代方案 SPSC lock-free queue 因 ABA + cache line 复杂度吃光红利，已回退**

### P1.1 HAP Q shader（YCoCg → RGB）

- OpenGL 2.0+ GLSL，运行时编译
- fragment shader：`R = Y - Co - Cg; G = Y + Cg; B = Y + Co - Cg`
- 驱动不支持 GLSL 时降级为 RGB 显示（颜色偏）
- 解决 HAP Q 直接当 RGB 显示的偏色问题

### P1.2 自适应帧率

- 实测 FPS < `targetFps * 60%` 时每 5 秒降 `g_maxFps` 5fps（最低 15fps）
- 实测 FPS > `targetFps * 95%` 时逐步回升到初始值
- 命令行 `--max-fps N` 可锁定上限

### P1.4 HAP 自带音频

- 队列扫描时取第一个 `audioStreamIdx >= 0` 的 HAP 层
- 用独立 `AVFormatContext` 读音轨，避免与视频解码线程抢同一 fmt
- 优先级：HAP 内置音频 > `[audio] source=` 配置 > 无音频

### P2.3 NVIDIA NVDEC 硬解（H.264）

- 双保险检测：`hasNvidiaGPU()` 看驱动 DLL + `av_hwdevice_iterate_types` 看 FFmpeg 是否编译 CUDA hwaccel
- `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA)` + `h264_cuvid` 解码器
- **4K H.264 30fps：CPU 占用 ~80% → ~8%（降 60%+）**
- NVDEC 帧格式 `AV_PIX_FMT_CUDA` 检测后 `av_hwframe_transfer_data` 转 CPU NV12 再 sws_scale
- 测试开关：环境变量 `HAPPLAYER_NO_NVDEC=1` 强制软解

### Vulkan 阶段 A + B + C + E（HAP Q SPIR-V + DXT 上传 + 逐帧多层绘制）

**模块定位**：可选加速路径，不替换 OpenGL 主路径。
- **阶段 A**（`vulkan_hapq.cpp/.h`）：HAP Q shader pipeline + SPIR-V 字节码（YCoCg DXT5 → RGB）
- **阶段 B**：DXT 压缩纹理上传（staging buffer → image），替代 `glCompressedTexImage2D`
- **阶段 C**（`vulkan_renderer.cpp/.h`）：单 surface 简化渲染管线（instance / device / swapchain / 全屏 quad）
  - **【2026-08-14 实践修正】** A–D 只验证编译。真播从阶段 E 开始。
- 启用方式：CMake 检测 `deps/vulkan/{include,lib}` 自动打开 `HAPPLAYER_HAS_VULKAN=1`；运行时需 `HAPPLAYER_USE_VULKAN=1`（init 失败直接退出，不能回退 GL）
- **阶段 E 已接通**：逐帧 DXT/RGBA 上传 + 多层 viewport；实测 640×360 H.264/HAP 锁定 30 fps

---

## 四、UX 优化

- **资源预加载到内存**：启动时把所有视频读到内存，消除冷启动卡顿
- **配置热重载**：`layers.txt` 修改后自动应用（无需重启）
- **健康监控 + 自动重启**：fps < 5 持续 5 秒主动退出，任务计划程序接管
- **窗口位置记忆**：`state.json` 持久化到 `%APPDATA%\happlayer\`
- **配置字段校验**：8 类错误友好提示（缺字段 / 路径不存在 / 参数越界 / ...）

---

## 五、开发友好小项

- **JSON bench 报告**：CI/CD 友好，结构化输出 `--json`
- **--log-level 参数**：`error/warn/info/debug` 四级
- **配置文件字段校验**：缺字段、路径不存在、参数越界都有友好提示
- **debug 输出**：snappy worker 编号、NVDEC 状态、HAP-Q shader 编译等

---

## 六、文档清单

| 文档 | 行数 | 大小 | 用途 |
|------|------|------|------|
| `README.md` | 175 | ~8 KB | 项目说明（功能特性 + 编译 + 运行 + 配置） |
| `docs/PROJECT_SUMMARY.md` | - | - | **本文档**：项目全貌 + 性能数据 + 关键经验 |
| `docs/USER_GUIDE.md` | 510 | ~16 KB | 中文用户指南（编译 / 配置 / 视频源准备 / 部署 / 排错 / 调优） |
| `docs/DEPLOYMENT.md` | 537 | ~14 KB | 现场部署流程 + 故障应急 + 备份策略 |
| `docs/TROUBLESHOOTING.md` | 660 | ~15 KB | 常见错误 + 诊断命令 + 解决步骤 |
| `docs/p2-design.md` | 586 | ~23 KB | P2 架构演进调研（多 GPU / Vulkan / NVDEC 深度方案） |
| `docs/CHANGELOG.md` | - | - | 版本变更历史（v1.0 + v1.1 + P0/P1/P2 模块记录） |
| `docs/changelog/2026-08-14-vulkan-stage-e.md` | - | - | 阶段 E 原子记录 + 反哺点 |

---

## 七、文件清单

### 7.1 源码（4545 行）

| 文件 | 行数 | 大小 | 说明 |
|------|------|------|------|
| `src/main.cpp` | 2210 | 100 KB | 主程序（解码线程 / 主循环 / UDP / 配置 / Vulkan 探测） |
| `src/audio.cpp` | 536 | 18 KB | SDL2 音频子系统（AudioStream + 漂移补偿） |
| `src/audio.h` | 146 | 5 KB | 音频 API |
| `src/hapdecode.h` | 173 | 6 KB | HAP snappy 解码（DXT1/DXT5/YCoCg DXT5） |
| `src/vulkan_hapq.cpp` | 637 | 30 KB | Vulkan 阶段 A+B+E（持久 staging + 手写 SPIR-V） |
| `src/vulkan_hapq.h` | 113 | 5 KB | `createTexture` / `updateTexture` / `recordCopyToImage` |
| `src/vulkan_renderer.cpp` | 626 | 24 KB | Vulkan 阶段 E 绘制（copy-before-renderpass + 多层 viewport） |
| `src/vulkan_renderer.h` | 104 | 4 KB | 阶段 E API |

### 7.2 构建 / 脚本

| 文件 | 大小 | 说明 |
|------|------|------|
| `CMakeLists.txt` | ~4 KB | 构建系统（snappy 源码构建 + FFmpeg/GLFW/SDL2/Vulkan 链接） |
| `scripts/install-autostart.bat` | ~2 KB | 注册表 Run 键写入 |
| `scripts/set-config.bat` | ~3 KB | 交互式配置 ExePath + ConfigPath |
| `scripts/uninstall-autostart.bat` | ~0.4 KB | 删除注册表项 |
| `scripts/validate_bat.py` | ~6 KB | 验证 .bat 编码（UTF-8 无 BOM + CRLF） |
| `scripts/write_bat.py` | ~6 KB | 写 .bat 工具（保证 UTF-8 无 BOM + CRLF） |
| `layers.txt` | 111 B | 默认配置（示例） |

### 7.3 运行时产物（`build/Release/`）

| 文件 | 大小 | 说明 |
|------|------|------|
| `happlayer.exe` | **372 KB** | 主可执行文件（Release 配置） |
| `avcodec-62.dll` | 97 MB | FFmpeg 视频解码 |
| `avdevice-62.dll` | 6.3 MB | FFmpeg 设备 |
| `avfilter-11.dll` | 124 MB | FFmpeg 滤镜 |
| `avformat-62.dll` | 20 MB | FFmpeg 解封装 |
| `avutil-60.dll` | 3.1 MB | FFmpeg 工具库 |
| `swresample-6.dll` | 487 KB | FFmpeg 音频重采样 |
| `swscale-9.dll` | 12 MB | FFmpeg 视频缩放/转换 |
| `SDL2.dll` | 1.6 MB | SDL2 动态库 |
| `hapq.frag.spv` | 1.2 KB | Vulkan HAP Q fragment shader SPIR-V 字节码 |
| `hapq.vert.spv` | 1.0 KB | Vulkan HAP Q vertex shader SPIR-V 字节码 |

**总运行时大小：~265 MB**（其中 90% 是 FFmpeg 滤镜和解码库）

### 7.4 测试 clip

| 文件 | 大小 | 说明 |
|------|------|------|
| `clips/layer1_bottom.mov` | 16 MB | 测试用 720p HAP |
| `clips/layer2_mid.mov` | 9 MB | 测试用 720p HAP |
| `clips/layer3_top.mov` | 2 MB | 测试用 720p HAP |
| `clips/test_hap.mov` | 261 KB | 测试用 HAP |
| `clips/test_h264.mp4` | 7.8 MB | 测试用 1080p H.264 |
| `clips/test_h264_small.mp4` | 635 KB | 测试用 H.264 |
| `clips/test_anim.mov` | 377 KB | 测试用 QTRLE Animation |
| `clips/audio_test.wav` | 706 KB | 测试用音频 |

---

## 八、性能基准（实测）

| 场景 | 解码速度 | 渲染 FPS | 备注 |
|------|---------|---------|------|
| 单层 HAP 1280×720 60fps 目标 | - | **~32 fps** | headless 环境，无 GPU 加速命令序列化 |
| 单层 HAP 1280×720 120fps 目标 | - | **~65 fps** | 同上 |
| 单层 HAP 720p（snappy 并行后） | **~212 fps** | - | 主线程解码速度（6.4× 提速） |
| 单层 HAP 1080p H.264 NVDEC 硬解 | - | **60+ fps** | N 卡自动启用 |
| 2 队列 × 10 层 4K HAP | - | **~30 fps** | headless 物理上限（OpenGL 命令序列化） |
| 15 层 4K HAP | - | **~8 fps** | headless 物理上限，真实 GPU 预期 **30+ fps** |

**基线（仅 sws_scale 软解，无 PBO 无 snappy 并行）vs v1.1 完整优化**：
- 单层 720p HAP：~5 fps → ~32 fps（6.4×）
- 4K 15 层：~2 fps → ~8 fps（4×），OpenGL 命令序列化上限；Vulkan 阶段 E 在 640×360 单层已锁 30 fps（4K 多层尚未重测）

---

## 九、关键经验教训

### 9.1 P0.2 snappy 误判：从"不是瓶颈"到"最大瓶颈"

理论分析认为 snappy 解压在主线程开销占比小，不构成瓶颈。**实测发现主线程 50% 时间花在 snappy 解压**——并行化后直接 6.4× 提速，成为本项目最大单点收益。

**教训**：**先有基线 profile，再做优化决策**。"理论上不重要"不等于"实际不重要"。

### 9.2 P0.3 SPSC lock-free 教训：复杂度吃光红利

目标：消除 `cv.wait` + mutex 抢占开销。实施：DecState 改造为 SPSC ring buffer，Frame 用 buffer pool 索引，移除 `cv.wait`。

**实测**：4K 30 层 60fps 场景出现间歇性卡顿（frame drop ~5%）。
- 第一性原理：mutex+cv.wait 在 60fps 30 层下不构成瓶颈（实测 critical section < 50μs）
- SPSC 引入 ABA 风险 + cache line 对齐问题，复杂度红利被吃光

**完整回退到 `mutex + std::vector<Frame> + cv.wait` 简单实现**，后续用 `shared_mutex` 做读并发分离。

**教训**：**并发改造以"实测瓶颈"为前提，不是"看起来更优雅"**。

### 9.3 条件编译策略：Vulkan 模块作为可选加速路径

Vulkan 集成完整但**不替换 OpenGL 主路径**：
- 默认编译开关 `HAPPLAYER_HAS_VULKAN` 由 CMake 检测 `deps/vulkan/` 决定
- 运行时开关 `HAPPLAYER_USE_VULKAN` 决定是否走 Vulkan 路径
- 即使 Vulkan 路径失败也不影响 OpenGL 主路径
  - **【2026-08-14 废弃】** 阶段 E 窗口已是 `GLFW_NO_API`，init 失败必须退出，不能回退 GL。未设该环境变量时 OpenGL 主路径不受影响。
- 这样现场部署可以**先验证 OpenGL 稳定**，再**逐步测试 Vulkan 加速**（测试机先开环境变量，失败则去掉后用 GL）

### 9.4 decState 共享锁与读写分离

- `std::shared_mutex` 提供并发读能力（多个 updateLayer 同时读）
- `std::condition_variable_any` 配合 shared_mutex（cv.wait 必须用 cv_any）
- 写操作（decoderRun）用 `unique_lock`，读操作（updateLayer）用 `shared_lock`
- 性能提升不大（之前不构成瓶颈），但**代码清晰度提升**，避免误用

### 9.5 切场黑屏：preRoll + 不重置 visible

切场瞬间短暂无画面（~200ms）。根因：`resetLayer` 重置 `visible` 标志，导致 GPU 纹理在预填完成前被画到屏幕。**修复**：保留 `visible` 不重置；`preRoll` 改为 5 秒（默认）保证 ring buffer 填满。

### 9.6 NVDEC 帧格式处理

NVDEC 解出 `AV_PIX_FMT_CUDA`（CUDA device memory），sws_scale 默认读 CPU 报错。**修复**：检测 `frame->format == AV_PIX_FMT_CUDA` 时用 `av_hwframe_transfer_data` 拷回 CPU NV12 再 sws_scale。

---

## 十、后续可做

| 项目 | 门槛 | 工作量 | 优先级 |
|------|------|--------|--------|
| **P2.1 多 GPU 分担**（WGL 多 RC + 静态分配） | NVIDIA / AMD / Intel 三家扩展名不同，要 `#ifdef` | 15-20 天 | 中（30+ 层场景才需要） |
| **P2.2 Vulkan 完整管线**（多层叠加 + 视频纹理上传 + resize） | Vulkan API 学习曲线陡 | 30 天 | **阶段 E 已接通逐帧上传**；resize / 多线程 CB 仍开放 |
| **P2.3 Vulkan + NVDEC 零拷贝**（CUDA memory → Vulkan image） | 需 P2.2 完成 + CUDA-Vulkan 互操作 | 7 天 | 高（4K H.264 性能翻倍潜力） |
| **P2.3 RGBA PBO 异步上传**（多 codec 场景） | OpenGL PBO 兼容性已验证 | 5 天 | 中（多 codec 软解场景） |
| **AV1 / VP9 NVDEC 硬解** | FFmpeg 6.0+ + RTX 30+ | 3 天 | 低（客户暂未提需求） |
| **Web 控制台**（替代 UDP 命令） | Node + WebSocket | 5 天 | 低（UDP 已够用） |
| **多语言**（英文版文档 + UI） | - | 5 天 | 低（内部项目暂不需要） |
| **macOS / Linux 移植** | macOS 无 DWM 圆角绕过 / Linux 无 NVDEC | 30+ 天 | 低（无现场需求） |

**已弃用**：
- P0.3 SPSC lock-free queue（已回退，详见 CHANGELOG v1.0）

---

## 十一、致谢

- **项目结构与用户**：Windows 展厅项目实战需求驱动
- **FFmpeg**：https://ffmpeg.org/ （gyan.dev full_build-shared 预编译包，含 NVDEC hwaccel）
- **OpenGL**：Khronos Group
- **GLFW**：https://www.glfw.org/
- **SDL2**：https://libsdl.org/
- **Vulkan**：Khronos Group
- **Snappy**：https://github.com/google/snappy （HAP 帧压缩）
- **NVIDIA Video Codec SDK**：https://developer.nvidia.com/video-codec-sdk
- **HAP 视频规范**：https://github.com/Vidvox/hap

---

## 十二、版本对照

| 版本 | 日期 | 关键变化 |
|------|------|---------|
| **v1.2** | 2026-08-15 | Vulkan 阶段 D（多层 + RGBA + UBO + descriptor pool 完整化，vulkan_renderer.cpp 30KB→46KB）+ 阶段 E（main.cpp 环境变量切换 Vulkan 渲染路径 + OpenGL fallback 安全） |
| **v1.1** | 2026-08-14 | snappy 并行（6.4×）+ shared_mutex + Vulkan 模块预集成 + NVDEC 完整路径 + UX 小项 |
| **v1.0** | 2026-08-13 | P0/P1 + P2.3 NVDEC + 开机自启三件套 + 4K 直角 + 防休眠 + UDP 控制 |

详见 `docs/CHANGELOG.md`。

---

### Vulkan 阶段 D + E（多层 + RGBA + 主路径切换）

> **v1.2 (2026-08-15) 增量**。阶段 D + E 接通。

#### 完成的工作

- **Vulkan 阶段 D**（`vulkan_renderer.cpp`）：多层 HAP Q 叠加 + RGBA 路径 + UBO + descriptor pool 完整化（30KB → 46KB）
- **Vulkan 阶段 E**：主循环通过 `HAPPLAYER_USE_VULKAN=1` 环境变量切换到 Vulkan 渲染路径
- **OpenGL 路径 fallback 安全**：默认仍 OpenGL，Vulkan 失败自动回退
- Layer 结构增加 4 个 Vulkan 字段（vkImageView / vkImage / vkImageMemory / vkAllocated）
- `upload()` 函数增加 Vulkan 早返回分支（仅首次上传，per-frame 留待后续）

#### 路径切换

```bash
# 默认 OpenGL
./happlayer.exe layers.txt

# Vulkan 路径（编译过 + fallback 安全）
HAPPLAYER_USE_VULKAN=1 ./happlayer.exe layers.txt
```

#### 文件清单（v1.2 新增 / 更新）

| 文件 | v1.1 | v1.2 | 说明 |
|------|------|------|------|
| `src/vulkan_renderer.h` | 4 KB | 5 KB | 暴露 inline 访问器（getDevice/getPhysDevice/getCommandPool/getGraphicsQueue） |
| `src/vulkan_renderer.cpp` | 24 KB | **46 KB** | 多层 + RGBA + descriptor pool + UBO |
| `src/main.cpp` | 100 KB | 100 KB | 条件编译集成 Vulkan 路径 |
| `src/vulkan_hapq.cpp` | 30 KB | 30 KB | 保持（阶段 A+B E 已稳定） |

#### 关键经验沉淀（v1.2）

1. **GLFW_NO_API 必须设**，否则 Vulkan surface 创建崩溃（Win32 AV）
2. **shader module 成功 ≠ pipeline 能建**——必须运行时验证（vkCreateGraphicsPipelines 返回 VK_SUCCESS 才算 OK）
3. **持久 staging buffer + 单 command buffer**：避免每帧 vkCreateImage / 多 CB 同步开销
4. **OpenGL / Vulkan 双路径**用环境变量切换（不改主路径，Vulkan 仅作可选加速）

#### 已沉淀到全局规则

- 内部规则 `vulkan-integration-pitfalls`
- 内部规则 `glfw-vulkan-no-api`
- 内部规则 `vulkan-shader-module-not-pipeline`
- 内部 pattern `vulkan-persistent-staging-upload`

#### Known Issues（v1.2）

- **Vulkan 路径简化版仅首次上传**——per-frame Vulkan 上传留 TODO
- **阶段 F（NVDEC + Vulkan 零拷贝）**未做（10+ 天工作量）
- 4K 多层 Vulkan 性能未重测（仍在 640×360 30fps 验证通过）

---

**文档版本**：v1.2
**最后更新**：2026-08-15
**维护者**：happlayer 项目组
