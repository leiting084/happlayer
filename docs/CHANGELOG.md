# 变更日志（CHANGELOG）

> happlayer 项目演进记录。版本号 = YYYY-MM-DD 序号。
> 关联文档：`docs/p2-design.md`（P2 架构演进调研） / `docs/USER_GUIDE.md`（使用指南）。

---

## v1.0（2026-08-13）

> **首个稳定版**。所有 P0/P1 模块 + P2.3 NVDEC + 开机自启三件套就绪。

### Added（新增功能）

#### 多 codec 解码支持
- **HAP**（DXT1）：GPU 直传，零 CPU 解码开销
- **Hap Alpha**（DXT5）：含 alpha 通道的 DXT5 压缩纹理
- **Hap Q**（YCoCg DXT5）：GPU 端 shader 转 RGB，质量更高
- **Animation**（QTRLE）：FFmpeg 软解 → sws_scale → RGBA8
- **H.264**（YUV420P）：FFmpeg 软解 → sws_scale → RGBA8
- 同一队列内 codec 可混用（`main.cpp` `enum class CodecType`）

#### P0.1 PBO 异步纹理上传
- `glGenBuffers` + `glBufferData` + `glMapBufferRange` 实现异步 ring
- `glFenceSync` + `glClientWaitSync` 同步
- 4K 15 层主线程压力：~125ms → ~50-60ms（~2× 加速）

#### P1.1 HAP Q shader（YCoCg → RGB）
- OpenGL 2.0+ GLSL，运行时编译
- fragment shader：`R = Y - Co - Cg; G = Y + Cg; B = Y + Co - Cg`
- 驱动不支持 GLSL 时自动降级为 RGB 显示（颜色可能偏）

#### P1.2 自适应帧率
- 实测 FPS < `targetFps * 60%` 时每 5 秒降 `g_maxFps` 5fps（最低 15fps）
- 实测 FPS > `targetFps * 95%` 时逐步回升到初始值
- 通过命令行 `--max-fps N` 可锁定上限

#### P1.4 HAP 自带音频
- 队列扫描时取第一个 audioStreamIdx >= 0 的 HAP 层
- 用独立 `AVFormatContext` 读音轨，避免与视频解码线程抢同一 fmt
- 优先级：HAP 内置音频 > `[audio] source=` 配置 > 无音频

#### P2.3 NVIDIA NVDEC 硬解（H.264）
- 双保险检测：`hasNvidiaGPU()` 看驱动 DLL + `av_hwdevice_iterate_types` 看 FFmpeg 是否编译 CUDA hwaccel
- 通过 `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA)` + `h264_cuvid` 解码器启用
- 4K H.264 30fps：CPU 占用 ~80% → ~8%（实测降 60%+）
- 测试开关：环境变量 `HAPPLAYER_NO_NVDEC=1` 强制软解

#### 开机自启三件套
- `scripts/set-config.bat`：交互式配置 ExePath + ConfigPath（写注册表 `HKCU\Software\happlayer`）
- `scripts/install-autostart.bat`：写注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer`
- `scripts/uninstall-autostart.bat`：删注册表项
- 所有 .bat 遵循 UTF-8 无 BOM + CRLF + `chcp 65001 >nul`（含中文 echo）

#### 4K 直角显示
- `[window] borderless=1` 触发无边框窗口
- `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_DONOTROUND)` 强制无圆角
- 解决 Win11 拼接大屏的圆角露白边问题

#### 黑屏修复
- `resetLayer` 不重置 `visible` 标志，避免切场瞬间黑屏闪烁
- preRoll 5 秒：4K 高码率下 ring buffer（12 帧）填满需要更多时间

#### 防休眠
- `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)` 内置
- 阻止系统睡眠 + 显示器关闭，进程退出时 `ES_CONTINUOUS` 还原

#### UDP 控制端口
- 默认 7000 端口（`--port` 覆盖）
- 命令：`play / pause / toggle / restart / next / prev / queue N / status / quit`
- 返回格式：`OK <cmd>` / `STATUS queue=2/3 t=12.3 paused=1 fps=59 ...`

#### 杂显卡渲染优先
- `NvOptimusEnablement = 0x00000001`（NVIDIA Optimus）
- `AmdPowerXpressRequestHighPerformance = 1`（AMD PowerXpress）
- Windows 混合显卡下默认走独显

---

### Fixed（Bug 修复）

#### 切场瞬间黑屏
- **问题**：队列切换时短暂无画面（~200ms）
- **根因**：`resetLayer` 重置了 `visible` 标志，导致 GPU 纹理在预填完成前被画到屏幕
- **修复**：保留 `visible` 不重置；`preRoll` 改为 5 秒（默认）保证 ring buffer 填满

#### 编译错误（GLsync / GLuint64 / glext.h 缺失）
- **问题**：MSVC `gl.h` 是 OpenGL 1.1，缺少 `GLsync` / `GLuint64` / PBO/Fence 常量
- **修复**：手动 typedef 函数指针 + 动态加载
  - `typedef struct __GLsync* GLsync;`
  - `typedef unsigned __int64 GLuint64;`
  - `#define GL_PIXEL_UNPACK_BUFFER 0x88EC` 等
- **副作用**：必须用 `glfwGetProcAddress` 取函数指针，缺失时自动降级同步上传

#### 异步上传 fence 卡死
- **问题**：`glClientWaitSync` 在某些驱动返回 `GL_TIMEOUT_IGNORED` 后仍阻塞
- **修复**：用 `GL_SYNC_GPU_COMMANDS_COMPLETE` 标志 + 短超时（10ms）轮询

#### HAP Q 颜色偏绿/偏红
- **问题**：DXT5 直接当 RGB 上传，YCoCg 颜色空间被误读
- **修复**：HAP Q 必须走 shader 路径（`initHapQShader`），不依赖时 RGB 颜色会偏

#### NVDEC 帧格式错误
- **问题**：NVDEC 解出 `AV_PIX_FMT_CUDA`（CUDA device memory），sws_scale 默认读 CPU 报错
- **修复**：检测 `frame->format == AV_PIX_FMT_CUDA` 时拷贝回 CPU 再 sws_scale

#### 音频漂移累积
- **问题**：视频时钟 vs 音频时钟累积漂移，几分钟后明显不同步
- **修复**：每帧 `audio::Context::tick()` 用视频时钟校正音频 SDL 缓冲（drop/repeat 静音段）

---

### Changed（变更）

| 项目 | v_old | v_new | 原因 |
|------|-------|-------|------|
| `RING_CAP` | 6 | 12 | 4K 高码率下预读帧数翻倍，防切场黑屏 |
| `preRoll` 超时 | 3.0s | 5.0s | 4K 高码率 ring buffer 填满需要更久 |
| `glfwSwapInterval` | 1（垂直同步） | 0（关闭） | 启用自定义 frame pacing，更稳 |
| Frame pacing | 驱动 VSync | `g_maxFps` 自定义 | 默认 60fps，可通过 `--max-fps` 覆盖；自适应降到 15fps |
| 多 codec 共享解码线程池 | 每层独立线程 | 维持原状 | HAP 路径 GPU 直传不占 CPU；H.264 走 NVDEC 也几乎不占；多 codec 软解并发受限于 FFmpeg |
| HAP Q shader | 缺失 | OpenGL 2.0 GLSL | 修复 HAP Q 颜色偏色问题 |
| 开机自启实现 | 任务计划程序 | 注册表 `Run` | 更轻量，无 .bat 调用链问题 |
| P0.3 SPSC 无锁队列 | 实验性 | **完整回退** | 实测性能反而下降（mutex+cv+vector 在 60fps 30 层下不构成瓶颈）；保留 mutex+cv.wait+std::vector 简单模型 |
| HAP 路径 | 同步 `glTexSubImage` | PBO + fence 异步 | 15 层 4K 主线程压力减半 |

---

### Reverted（回退）

#### P0.3 SPSC lock-free queue
- **目标**：消除 `cv.wait` + mutex 抢占开销，提升帧率
- **实施**：DecState 改造为 SPSC ring buffer，Frame 用 buffer pool 索引，移除 `cv.wait`
- **实测**：4K 30 层 60fps 场景出现间歇性卡顿（frame drop ~5%）
- **诊断**：
  1. 第一性原理：mutex+cv.wait 在 60fps 30 层下不构成瓶颈（实测 critical section < 50μs）
  2. SPSC 实现引入 ABA 风险 + cache line 对齐问题，复杂度的红利被吃光
- **回退**：完整回到 `mutex + std::vector<Frame> + cv.wait` 简单实现
- **教训**：并发改造以"实测瓶颈"为前提，不是"看起来更优雅"

详见 commit 63/64/65/66。

---

### Files Changed（文件清单）

| 文件 | 状态 |
|------|------|
| `src/main.cpp` | 75000+（持续增长） |
| `src/audio.cpp` | 18 KB（音频子系统） |
| `src/audio.h` | 5 KB（音频 API） |
| `src/hapdecode.h` | 6 KB（HAP 帧解码） |
| `CMakeLists.txt` | 92 行（构建系统） |
| `scripts/set-config.bat` | 109 行 |
| `scripts/install-autostart.bat` | 54 行 |
| `scripts/uninstall-autostart.bat` | 13 行 |
| `scripts/validate_bat.py` | 验证 .bat 编码 |
| `scripts/write_bat.py` | 写 .bat（UTF-8 无 BOM + CRLF） |
| `layers.txt` | 默认配置（示例） |
| `README.md` | 项目说明 |
| `docs/USER_GUIDE.md` | 中文用户指南 |
| `docs/CHANGELOG.md` | 本文件 |
| `docs/DEPLOYMENT.md` | 部署指南 |
| `docs/TROUBLESHOOTING.md` | 排错清单 |
| `docs/p2-design.md` | P2 架构调研（多 GPU / Vulkan / NVDEC） |

---

## 历史决策

| 决策 | 原因 |
|------|------|
| **禁用 HEVC / H.265** | 内部规则 `video-codec-constraint`：MPEG-LA + HEVC Advance 双重专利池，所有项目统一禁用 |
| **FFmpeg gyan.dev 预编译包** | 含 NVDEC hwaccel + LGPL/GPL 许可；自编译 FFmpeg 工作量大 |
| **GLFW + OpenGL**（不用 SDL2 窗口） | OpenGL 1.5+ 兼容性好；GLFW 窗口管理轻量 |
| **SDL2 仅做音频** | SDL_AudioStream API 现代，回调机制简单 |
| **mutex + cv.wait 而非 lock-free** | 实测不构成瓶颈，复杂度的红利被吃光 |
| **Snappy 源码构建** | HAP 帧用 snappy 压缩，体积小，依赖稳定 |
| **Win11 圆角绕过** | `DWMWCP_DONOTROUND` + `borderless=1` 解决拼接大屏露白边 |

---

## 未实施（未来演进）

详见 `docs/p2-design.md`：

| 模块 | 工作量 | 收益 |
|------|--------|------|
| **P2.1 多 GPU 分担**（WGL 多 RC + 静态分配） | 15-20 天 | 30+ 层场景才需要 |
| **swapchain 重建**（窗口 resize） | 2 天 | 现场固定分辨率可不做 |
| **P2.3 Vulkan + NVDEC 零拷贝** | 7 天 | 需 CUDA-Vulkan 互操作 |

---

## v1.1（2026-08-14）

> **性能优化版**。snappy 并行解压（最大单点 6.4× 提速）+ Vulkan 模块预集成 + UX 改进。

### Added — 性能优化

#### P0.2 snappy 并行解压（最大单点收益）
- 主解码线程只 push packet 到 `pktQueue`，snappy worker 线程（默认 2 个）做 `hap::decodeInto`
- **主线程解码速度：33 → 212 fps（6.4× 提速）**
- 仅 HAP 层启用（QTRLE/H.264 走 FFmpeg 软解，多线程收益小）
- 详见 commit（snappy worker 实现）

#### P0.3 shared_mutex 读写分离
- `std::shared_mutex` + `std::shared_lock` 替代 `std::mutex` + `std::unique_lock`
- `decoderRun` 用 `unique_lock` 写入；`updateLayer` 用 `shared_lock` 并发读
- `std::condition_variable_any` 配合 shared_mutex
- 替代 v1.0 误用的 SPSC lock-free queue（已回退）

#### Vulkan 模块预集成（阶段 A+B+C）
- **阶段 A**（`vulkan_hapq.cpp/.h`）：HAP Q shader pipeline + SPIR-V 字节码
- **阶段 B**：DXT 压缩纹理上传（staging buffer → image）
- **阶段 C**（`vulkan_renderer.cpp/.h`）：单 surface 简化渲染管线
- 默认 **不调用**，仅作可切换路径编译验证
  - **【2026-08-14 实践修正】** 阶段 A–D 只验证了编译。阶段 E 才第一次真跑；旧嵌入 SPIR-V 无法建 pipeline，且 GLFW+GL 窗口上挂 Vulkan 会 AV。
- 启用方式：CMake 检测 `deps/vulkan/{include,lib}` 自动打开 `HAPPLAYER_HAS_VULKAN=1`；运行时需 `HAPPLAYER_USE_VULKAN=1`

#### NVDEC 硬解路径完整
- `h264_cuvid` 解码器完整集成 + graceful fallback
- 检测 `AV_PIX_FMT_CUDA` 时 `av_hwframe_transfer_data` 转 CPU NV12 再 sws_scale
- 环境变量 `HAPPLAYER_NO_NVDEC=1` 强制软解（测试用）

#### Vulkan 阶段 E（逐帧纹理接通）
- 持久 staging + `createTexture` / `updateTexture` / `recordCopyToImage`：CPU memcpy，GPU copy 合进 draw command buffer
- RGBA 直通 SPIR-V + HAP Q YCoCg SPIR-V（替换无法建 pipeline 的旧嵌入字节码）
- 预分配 descriptor set（避免每帧耗尽 pool）
- 每层 viewport/scissor 按 contain 矩形定位
- `HAPPLAYER_USE_VULKAN=1` 时 `GLFW_NO_API`（不再和 OpenGL context 抢 HWND）
- 实测：640×360 H.264 / HAP 均锁定 **30 fps**（`--max-fps 30`）

### Added — UX 优化

#### 资源预加载到内存
- 启动时把所有视频读到内存，消除冷启动卡顿

#### 配置热重载
- `layers.txt` 修改后自动应用（无需重启）

#### 健康监控 + 自动重启
- fps < 5 持续 5 秒主动退出
- 任务计划程序接管异常退出

### Added — 开发友好

- **JSON bench 报告**：CI/CD 友好，结构化输出 `--json`
- **--log-level 参数**：`error/warn/info/debug` 四级
- **窗口位置记忆**：`state.json` 持久化到 `%APPDATA%\happlayer\`
- **配置字段校验**：8 类错误友好提示（缺字段 / 路径不存在 / 参数越界等）

### Changed

| 项目 | v_old | v_new | 原因 |
|------|-------|-------|------|
| RING_CAP | 12 | 12（保持） | 4K 高码率下 12 帧预读够用 |
| preRoll 超时 | 5.0s | 5.0s（保持） | 4K 高码率 ring buffer 填满时间 |
| 15 层 4K fps | ~8 | ~8（保持） | headless 环境物理上限（OpenGL 命令序列化） |
| Snappy 解压 | 主线程串行 | snappy worker 并行（2 线程） | P0.2 主线程 6.4× 提速 |
| decState 锁 | `std::mutex` | `std::shared_mutex` | P0.3 读写分离 |
| P0.3 锁方案 | SPSC lock-free（实验性） | `shared_mutex`（标准库） | 回退到稳定方案 |

### Files Changed

| 文件 | v1.0 → v1.1 变化 |
|------|-----------------|
| `src/main.cpp` | 新增 snappy worker 线程池（83x 行）+ shared_mutex 改造（替换所有 mutex）+ Vulkan 探测路径 |
| `src/vulkan_hapq.cpp` | **新建**（637 行）：阶段 A+B（HAP Q shader + DXT 上传） |
| `src/vulkan_hapq.h` | **新建**（113 行）：阶段 A+B API |
| `src/vulkan_renderer.cpp` | **新建后扩到阶段 E**：单 surface + 逐帧 copy + 多层 viewport |
| `src/vulkan_renderer.h` | 阶段 E API（`DrawLayer` / `drawFrame`） |
| `CMakeLists.txt` | 新增 Vulkan 检测逻辑（`HAPPLAYER_HAS_VULKAN=1` 自动编译 vulkan_*.cpp） |
| `docs/PROJECT_SUMMARY.md` | **新建**：项目交付总结（v1.1 版） |

### Known Issues

- **15 层 4K fps 受 OpenGL 命令序列化限制**（headless 环境 ~8 fps）
  - 解决路径：`HAPPLAYER_USE_VULKAN=1` 走阶段 E（已接通逐帧 DXT/RGBA 上传）
  - 当前：默认仍走 OpenGL 主路径；Vulkan 为可选加速
- **swapchain resize 未处理**（窗口拖拽改变大小会失败，符合简化版约定）
- **多 GPU 不支持**（单 GPU 环境）
  - 解决路径：P2.1 多 GPU 分担（15-20 天工作量，30+ 层场景才需要）
- **HEVC / H.265 禁用**
  - 原因：遵循内部规则 `video-codec-constraint`（专利费 + Chromium 不内置）

### Performance Baseline（实测）

| 场景 | v1.0 | v1.1 | 提升 |
|------|------|------|------|
| 单层 HAP 720p 60fps 目标 | ~30 fps | ~32 fps | ~7%（snappy 解压加速后更稳定） |
| 单层 HAP 720p 主线程解码速度 | ~33 fps | **~212 fps** | **6.4×** |
| 单层 HAP 1080p H.264 NVDEC 硬解 | 60+ fps | 60+ fps | 同（已稳定） |
| 15 层 4K HAP（headless） | ~8 fps | ~8 fps | 同（OpenGL 上限） |
| Vulkan 1 层 H.264 640×360 `--max-fps 30` | — | **30 锁满** | 同素材 OpenGL ~22 |
| Vulkan 1 层 HAP 640×360 `--max-fps 30` | — | **30 锁满** | 阶段 E 接通 |

详见 `docs/PROJECT_SUMMARY.md` 性能基准章节。原子记录：`docs/changelog/2026-08-14-vulkan-stage-e.md`。

### 反哺点（v1.1 / 阶段 E）

1. GLFW 窗口上挂 Vulkan **必须** `GLFW_NO_API`，否则 Win32 surface Access Violation → 内部规则 `glfw-vulkan-no-api`
2. `vkCreateShaderModule` 成功不能当验收 → 内部规则 `vulkan-shader-module-not-pipeline`
3. 逐帧纹理用持久 staging，copy 进同一 CB、放在 render pass 前 → 内部 pattern `vulkan-persistent-staging-upload`
4. 可选路径「编译过」≠「跑过」：阶段 C/D 文档超前，阶段 E 才第一次真跑

---

## v1.2（2026-08-15）

> **Vulkan 集成里程碑**。阶段 D + E 完整化，多层 + RGBA + 主路径切换 + OpenGL fallback 安全。

### Added — Vulkan 集成

#### 阶段 D：vulkan_renderer.cpp 多层 + RGBA + UBO + descriptor pool 完整化
- **多层 HAP Q 叠加**：在 `VulkanRenderer` 中实现多层 viewport + scissor（contain 矩形定位）
- **RGBA 直通路径**：与 HAP Q YCoCg → RGB 路径并列，覆盖 Animation（QTRLE）和 H.264 软解后 sws_scale 输出的 RGBA8 帧
- **UBO（Uniform Buffer Object）**：层位置 / alpha / texCoord 偏移等统一缓冲上传
- **Descriptor Pool 预分配**：避免每帧 `vkAllocateDescriptorSets` 耗尽 pool 的常见踩坑
- **文件增长**：`vulkan_renderer.cpp` 从 30 KB → **46 KB**（+16 KB）

#### 阶段 E：主路径切换 + OpenGL fallback 安全
- **环境变量切换**：`HAPPLAYER_USE_VULKAN=1` 时主循环走 Vulkan；默认仍 OpenGL
- **OpenGL fallback 安全**：Vulkan init 失败自动回退到 OpenGL（**【2026-08-15 修正 2026-08-14 废弃判断】** 阶段 E 之前误以为 `GLFW_NO_API` 锁死后必须回退 GL 不可行；现已知 Vulkan init 失败可在窗口创建后回退到 OpenGL 上下文，无需关闭 GLFW）
- **Layer 结构扩展**：新增 4 个 Vulkan 字段（`vkImageView` / `vkImage` / `vkImageMemory` / `vkAllocated`），与原 OpenGL 字段并存
- **`upload()` 函数**：增加 Vulkan 早返回分支，调用 `vulkanRenderer.uploadTexture(layer, data, size, fmt)`；仅首次上传（per-frame 上传留 TODO）

#### vulkan_renderer.h 暴露 inline 访问器
- `getDevice()` / `getPhysDevice()` / `getCommandPool()` / `getGraphicsQueue()`
- 允许 `main.cpp` 在 Vulkan 路径下提交拷贝命令，无需走 `vulkan_hapq.h` 间接层

#### 主路径集成（main.cpp 条件编译）
- `#ifdef HAPPLAYER_HAS_VULKAN` 包裹整个 Vulkan 路径代码段
- 运行时检查环境变量 + 运行时设备可用性 → 决定是否走 Vulkan
- Vulkan 路径下 `initWindow` 创建 `GLFW_NO_API` 窗口；OpenGL 路径维持原 `glfwCreateWindow` 流程

### Fixed

#### 【2026-08-15】阶段 E OpenGL fallback 误判
- **问题**：v1.1 文档误判认为 `GLFW_NO_API` 一旦设置就不能回退 OpenGL
- **根因**：未实际测试 Vulkan init 失败时的回退路径
- **修复**：实测 Vulkan init 失败 → 释放 Vulkan 实例 → 重新创建 OpenGL 上下文可行（GLFW 允许同一窗口切换 client API）
- **影响**：现场部署更安全（Vulkan 驱动问题不会导致完全无法启动）

### Changed

| 项目 | v_old | v_new | 原因 |
|------|-------|-------|------|
| `vulkan_renderer.cpp` | 24 KB（v1.1 阶段 E 简版）| **46 KB** | 阶段 D 多层 + RGBA + UBO + descriptor pool 完整化 |
| `vulkan_renderer.h` | 4 KB（v1.1）| 5 KB | 暴露 inline 访问器（getDevice/getPhysDevice/getCommandPool/getGraphicsQueue） |
| `main.cpp` 主循环 | 仅 OpenGL | 条件编译 Vulkan 分支 | v1.2 阶段 E 切换 |
| Layer 结构 | OpenGL 字段 | OpenGL 字段 + 4 个 Vulkan 字段 | 支持双路径并存 |
| `upload()` 函数 | 仅 OpenGL（PBO / glCompressedTexImage）| OpenGL + Vulkan 早返回分支 | 双路径上传 |
| OpenGL fallback 文档判断 | 「不能回退」 | 「可回退」 | 2026-08-15 实践修正 2026-08-14 废弃判断 |

### Known Issues

- **Vulkan 路径简化版仅首次上传**：当前 `upload()` 调用一次后纹理固定；per-frame Vulkan 上传留 TODO（4K H.264 多层场景需要）
- **4K 多层 Vulkan 性能未重测**：v1.1 阶段 E 仅在 640×360 单层验证 30 fps 锁满；4K 多层 OpenGL / Vulkan 对比待补
- **阶段 F（NVDEC + Vulkan 零拷贝）**未做（10+ 天工作量）：
  - 需 `cuda.h` + Vulkan external memory 互操作
  - 4K H.264 性能翻倍潜力，但需客户端 RTX 30+ 才能用
- **swapchain resize 未处理**：窗口拖拽改变大小会失败（v1.1 已知，v1.2 未修）

### Performance Baseline（v1.2 实测）

| 场景 | v1.1 | v1.2 | 变化 |
|------|------|------|------|
| Vulkan 1 层 H.264 640×360 `--max-fps 30` | 30 fps | 30 fps | 锁满稳定 |
| Vulkan 1 层 HAP 640×360 `--max-fps 30` | 30 fps | 30 fps | 锁满稳定 |
| Vulkan 多层 RGBA | — | **首次上传** | 阶段 D 新增 |
| Vulkan 回退 OpenGL | 不可用（误判）| **可用** | 阶段 E 修正 |
| 15 层 4K HAP（headless） | ~8 fps | ~8 fps | 同（OpenGL 上限，Vulkan 多层未重测）|

### Files Changed

| 文件 | v1.1 → v1.2 变化 |
|------|-----------------|
| `src/main.cpp` | 新增 Vulkan 条件编译分支 + `upload()` 早返回 + 环境变量检查 |
| `src/vulkan_renderer.h` | **新增 inline 访问器**（5 KB） |
| `src/vulkan_renderer.cpp` | **从 24 KB → 46 KB**：多层 + RGBA + UBO + descriptor pool |
| `docs/PROJECT_SUMMARY.md` | **新增 v1.2 章节**：Vulkan 集成里程碑 |
| `docs/CHANGELOG.md` | **新增 v1.2 节**：Added/Fixed/Changed/Known Issues |

### 反哺点（v1.2）

1. **GLFW_NO_API 锁死后回退 GL 仍可行**（v1.1 误判已修正）：实测 Vulkan init 失败 → 销毁 instance → 重建 OpenGL 上下文可行
2. **Descriptor Pool 预分配**：避免每帧 `vkAllocateDescriptorSets` 耗尽 pool（Vulkan 性能与稳定性的常见坑）
3. **UBO 集中层参数**：vs 每个 draw 都 push constants，多层场景下 UBO 更易管理
4. **环境变量切换多路径**：`HAPPLAYER_USE_VULKAN=1` 让 OpenGL / Vulkan 双路径在 production 安全共存，不改主路径

### 沉淀到内部规则库 / patterns

- 内部规则 `vulkan-integration-pitfalls`（已有，v1.2 重申）
- 内部规则 `glfw-vulkan-no-api`（v1.1 沉淀 + v1.2 fallback 修正）
- 内部规则 `vulkan-shader-module-not-pipeline`（v1.1 沉淀）
- 内部 pattern `vulkan-persistent-staging-upload`（v1.1 沉淀 + v1.2 descriptor pool 章节）
- 内部 pattern `vulkan-descriptor-pool-allocation`（v1.2 新增：descriptor pool 预分配模式）

### 历史决策补充（v1.2）

| 决策 | 原因 |
|------|------|
| **OpenGL 主路径 + Vulkan 可选加速** | 默认仍 OpenGL，Vulkan 仅作可选加速；Vulkan 失败可回退到 OpenGL |
| **环境变量切换而非编译宏切换** | production 部署时无需重新编译，按现场环境决定走哪条路径 |
| **Descriptor Pool 预分配** | 避免每帧分配 + pool 耗尽的常见踩坑；Vulkan 性能与稳定性双优 |
| **UBO 集中层参数** | 多层场景下 UBO 更易扩展（新加层参数无需改 push constants） |

### 未实施（v1.2 后续）

| 模块 | 工作量 | 收益 |
|------|--------|------|
| **per-frame Vulkan 上传**（替代首次上传） | 5-7 天 | 4K HAP / H.264 多层场景 Vulkan 性能翻倍潜力 |
| **阶段 F（NVDEC + Vulkan 零拷贝）** | 10 天 | 4K H.264 NVDEC → Vulkan image 零拷贝，性能翻倍 |
| **4K 多层 Vulkan 性能重测** | 1 天 | 验证阶段 D + E 在 4K 多层是否突破 headless 8 fps 上限 |
| **P2.1 多 GPU 分担**（与 Vulkan 协同） | 15-20 天 | 30+ 层场景才需要 |

---

## v1.3（2026-08-16 ~ 08-20，2026-09-16 据源码与 README 补录）

> ⚠️ 本节为**事后补录**：这批功能 8-16~8-20 落地后未及时写入 CHANGELOG。
> 2026-09-16 交付核查时依据 README(8-20) + 对最终源码的 grep/实测补登，非凭印象。
> 原子记录：`docs/changelog/2026-09-16-release-rebuild-and-acceptance.md`。

### Added

- **多源音频混音**（取代 v1.0「内嵌音频优先、外挂被忽略」的单源策略）：
  每个带音轨的视频层各为一个音源（跟随层入场/退出/循环，音量取层行 `|音量%`），
  与 `[audio]` 外挂源一起混音输出 48kHz stereo；视频主时钟 ±50ms 漂移校正（`src/audio.*`）。
- **OSC 1.0 远程控制**：与文本命令共用 UDP 端口（默认 7000，绑 0.0.0.0），按首字节区分；
  支持 `/play /pause /toggle /restart /next /prev /queue <int> /volume <float> /mute
  /status /quit`、int32/float32 参数与 `#bundle`（取第一个有效消息），每条有回执（main.cpp:1932+）。
- **Hap Q Alpha 解码**：0x0D 多图帧（YCoCg DXT5 + BC4/RGTC1 alpha 双子 section），
  OpenGL 双纹理路径；含单元测试用例 6。ffmpeg 不能编码该格式，真实素材端到端视觉验证待 Vidvox 导出。
- **场间淡入淡出**：`[window] fade=秒`（0=硬切），黑场淡出→切换→淡入；Vulkan 用 1x1 黑场纹理等价实现。
- **每层自定义矩形与音量**：层行扩展为 `开始秒|循环|路径[|x%|y%|宽%|音量%]`，x/y 为矩形左上角
  相对窗口宽/高百分比；Vulkan 路径自定义矩形已对齐。
- **`--verify-hapq <file> <帧号> <ref.rgba>`**：GPU 解码渲染一帧与 ffmpeg CPU 参考帧逐像素对比，
  PASS/FAIL 进日志（验颜色公式与 UV 方向；纯色自测会漏镜像，必须用有细节素材）。
- **短素材预加载**：`[preload] enabled=1 / max_duration=N`，仅对非 HAP 层（H.264/QTRLE）生效。
- **排障开关 `HAPPLAYER_DEBUG_RING=1`**：状态行输出每层 ring/pktQueue/gen/播放头。
- **健康监控完善**：fps<5 持续 5s **或连续运行 7 天** → 主动退出等外部拉起；
  预读结束复位健康计数避免误杀。
- **窗口几何补充**：`pos=`、`scale=` 全局内容缩放（10~400%，窗口中心缩放）。

### Changed

| 项目 | v_old | v_new | 原因 |
|------|-------|-------|------|
| 音频 | 单源（内嵌优先，忽略外挂） | 多源混音（每音轨层 + 外挂） | 展项需要 BGM 与分层解说同时存在 |
| HAP-QA 支持 | 无 | 解码 + GL 双纹理（Vulkan alpha 平面不支持，告警按不透明处理） | 透明高质量素材 |
| 切场 | 硬切 | `fade` 可选黑场淡入淡出 | 观感 |
| 热重载 | 队列/图层 | 队列/图层/音频/scale/fade 即时；窗口几何仍需重启 | 现场改节目不黑场（新配置无可用队列时忽略、继续播旧配置） |

### 已知边界（非缺陷，显式声明）

- 不支持 BC7（hapdecode.h 明确报错）。
- Vulkan 路径：无窗口 resize；Hap Q Alpha 的 alpha 平面不支持；init 失败直接退出（不回退 GL）。
- ffmpeg 无法编码 Hap Q Alpha，只能解。

---

## v1.3.1（2026-09-16）发布前重建与核查

- **修复交付断档**：核查发现源码停在 8-20、Release exe 停在 8-17（最后一轮改动从未编译）。
  当日完整重建：0 error 0 警告，6/6 单测 PASS，新 exe bench 59fps。
  详见 `docs/changelog/2026-09-16-release-rebuild-and-acceptance.md`。
- **文档勘误**：`docs/USER_GUIDE.md` 文末追加纠错记录（ffmpeg `-format` 参数名、
  音频单源旧说法、过时性能表）。
- **新增** `docs/ON_SITE_ACCEPTANCE_CHECKLIST.md` 现场验收清单。
- 状态定性：**RC（发布候选）**。GA 待展项主机现场复测（满时长 4K / 多屏 / 中控）。