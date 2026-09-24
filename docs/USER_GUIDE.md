# happlayer 用户使用指南

> 中文用户指南。覆盖：编译、配置、视频源准备、部署、排错、性能调优。

---

## 目录

1. [安装与编译](#1-安装与编译)
2. [配置文件 layers.txt](#2-配置文件-layerstxt)
3. [视频源准备](#3-视频源准备)
4. [部署：开机自启 / 4K 直角 / 防休眠](#4-部署开机自启--4k-直角--防休眠)
5. [排错清单](#5-排错清单)
6. [性能调优](#6-性能调优)（含 [6.6 Vulkan 可选加速](#66-vulkan-可选加速阶段-e)）

---

## 1. 安装与编译

### 1.1 环境准备

| 工具 | 版本 | 用途 |
|------|------|------|
| Visual Studio 2022 | 17.x | C++17 编译（MSVC v143） |
| CMake | 3.16+ | 构建系统 |
| Git for Windows | 最新 | 拉取可选（项目本身非 git 仓库） |
| NVIDIA 驱动 | 521.xx+ | NVDEC 硬解（可选但强烈推荐） |

### 1.2 依赖目录（`deps/`）

```
deps/
├── ffmpeg/                # gyan.dev full_build-shared 预编译（已包含）
│   ├── bin/               # 8 个 av*.dll + swscale/swresample + ffmpeg.exe
│   ├── include/           # 头文件
│   └── lib/               # 导入库
├── glfw/                  # 预编译库（lib-vc2022 / lib-mingw-w64）
├── sdl2/                  # 预编译开发包（含 cmake config）
├── snappy/                # 源码构建（CMakeLists 已配置 add_subdirectory）
└── vulkan/                # 可选：header + lib；CMake 检测到则 HAPPLAYER_HAS_VULKAN=1
```

依赖已全部包含在 `deps/` 下，**不需要联网**。

### 1.3 编译步骤

**MSVC（推荐，Windows 桌面）**：

```bash
cd 项目根目录
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物路径：`build\Release\happlayer.exe`

**MinGW**：

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

产物路径：`build\happlayer.exe`

### 1.4 编译验证

```bash
cd build\Release
.\happlayer.exe --bench 5            # 跑 5 秒应能启动 → 自动退出（无 layers.txt 会报错）
```

预期输出片段：

```
检测到 1 台显示器：
  1: 2560x1600 @240Hz  虚拟坐标(0,0)  [主]
...
>>> 队列 1/1 开始
[benc] 5s 结束，平均 FPS ≈ 60
```

---

## 2. 配置文件 `layers.txt`

配置文件默认路径：`happlayer.exe` 同目录下的 `layers.txt`。
命令行第一个参数可覆盖：`happlayer.exe my-config.txt`。

### 2.1 文件结构

```ini
# 注释以 # 开头，整行有效

[window]    ← 窗口/显示段（可整段删掉用默认值；命令行参数优先）
size=3840x2160
borderless=1
...

[audio]     ← 音频段（可选；HAP 不带音频时必须外挂）
source=...
volume=0.8
loop=0

[queue]     ← 队列 1
0|1|clips/layer1.mov
2|0|clips/layer2.mov

[queue]     ← 队列 2
0|1|clips/layer3.mov
```

**段顺序无关**（解析器按段名识别）。但 `[queue]` 段可以有多个，按出现顺序编号 1, 2, 3...。

### 2.2 `[window]` 段字段

| 字段 | 默认 | 说明 |
|------|------|------|
| `size=WxH` | 640x480 | 窗口尺寸。**4K 直角必须 3840x2160**；跨屏拼接写总尺寸（如双 1080p 横屏: 3840x1080） |
| `pos=X,Y` | 0,0 | 窗口左上角相对**目标显示器**左上角的偏移 |
| `monitor=N` | 1 | 从第几个显示器开始（1 起；启动日志里有编号列表） |
| `borderless=0/1` | 0 | 无边框窗口。**4K 直角必须 1**（否则 Win11 圆角 + 白边） |
| `scale=N` | 100 | 内容缩放百分比（以窗口中心缩放，100 = 原始适配） |
| `fullscreen=0/1` | 0 | 启动即全屏（占满目标显示器；F 键可切换） |

### 2.3 `[audio]` 段字段（可选）

| 字段 | 默认 | 说明 |
|------|------|------|
| `source=路径` | — | 音频文件路径（mp3/wav/aac/ogg） |
| `volume=0.0~1.0` | 1.0 | 音量；>1 会削顶 |
| `loop=0/1` | 0 | 是否循环 |

**优先级**：

```
HAP 文件自带音频流  >  [audio] source 配置  >  无音频
```

也就是说，如果你用 HAP 文件且带音轨，**优先用其内置音频**，配置 `[audio]` 段会被忽略（除非所有 HAP 都不带音频）。

### 2.4 `[queue]` 段层格式

```
开始秒|循环(0/1)|视频路径
```

- **开始秒**：在队列时间轴的哪一秒让该层入场（0 表示立即入场）
- **循环**：1 = 循环播放（永远不会"播完"），0 = 播完自动退出
- **视频路径**：相对 `happlayer.exe` 所在目录，支持 `/` 或 `\` 分隔

**队列结束条件**：队列中**所有不循环层**都播完 → 自动切下一队列。最后一个队列播完 → 回到队列 1。

**示例**：

```ini
[queue]
# 第一场：底层循环背景 + 2 秒入场的方块层（播完退出）+ 5 秒入场的绿块
0|1|clips/layer1_bottom.mov      # 底层永远在
2|0|clips/layer2_mid.mov         # 2 秒入场，5 秒后播完退出
5|0|clips/layer3_top.mov         # 5 秒入场，N 秒后播完退出

[queue]
# 第二场：方块层做底循环 + 1 秒入场的绿块（6 秒播完 → 整场结束）
0|1|clips/layer2_mid.mov
1|0|clips/layer3_top.mov
```

**层顺序 = 从下往上叠**（第一行在最底层，最后一行在最顶层）。

---

## 3. 视频源准备

### 3.1 HAP 编码（推荐，GPU 直传）

HAP 是 Vidvox 设计的**GPU 友好**格式：DXT 压缩纹理直传 GPU，零 CPU 解码开销，4K 视频可 60fps 多层。

**ffmpeg 命令**（HAP 基本，无 alpha）：

```bash
deps\ffmpeg\bin\ffmpeg.exe -y -i input.mp4 \
    -c:v hap -format hap -pix_fmt rgb \
    -c:a copy \
    clips\output_hap.mov
```

**Hap Alpha**（带 alpha 通道，DXT5）：

```bash
deps\ffmpeg\bin\ffmpeg.exe -y -i input_with_alpha.mov \
    -c:v hap -format alpha_hap -pix_fmt rgba \
    clips\output_alpha_hap.mov
```

**Hap Q**（YCoCg DXT5，GPU 端 shader 转 RGB；同等画质更小体积）：

```bash
deps\ffmpeg\bin\ffmpeg.exe -y -i input.mp4 \
    -c:v hap -format q_hap -pix_fmt rgba \
    clips\output_hap_q.mov
```

> Hap Q 需要 OpenGL 2.0+ GLSL 支持（驱动 ≥ 2017 年都支持）。

**验证编码**：

```bash
deps\ffmpeg\bin\ffprobe.exe -show_streams clips\output_hap.mov
# 看 codec_name = hap 或 hap_alpha 或 hap_q
```

### 3.2 HAP + H.264 混用

同一播放器支持混 codec：底层用 HAP（高效），某些特效层用 H.264（兼容性好）。但 H.264 软解路径不走 PBO 上传，性能受限。

```ini
[queue]
0|1|clips/background_hap.mov      # HAP — GPU 直传
2|0|clips/overlay_h264.mp4        # H.264 — FFmpeg 软解 + sws_scale
```

**H.264 转码命令**（标准 HAP 兼容）：

```bash
deps\ffmpeg\bin\ffmpeg.exe -y -i input.mp4 \
    -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
    -c:a aac -b:a 192k \
    clips\overlay_h264.mp4
```

### 3.3 Animation (QTRLE) 替代

QTRLE（QuickTime Animation）是 Apple 的无损 RGB 编码，FFmpeg 支持软解。适合**透明视频**但体积大（4K 1 分钟 ≈ 几 GB）。

**ffmpeg 命令**（QTRLE + alpha）：

```bash
deps\ffmpeg\bin\ffmpeg.exe -y -f lavfi -i "color=red:size=1920x1080:rate=30:duration=5" \
    -f lavfi -i "color=blue:size=1920x1080:rate=30:duration=5" \
    -filter_complex "[0][1]blend=all_mode=addition[v]" \
    -map "[v]" -c:v qtrle -pix_fmt rgba \
    clips\test_anim.mov
```

### 3.4 音频：外挂 vs HAP 自带

**外挂 wav/mp3**（HAP 不带音频，必须外挂）：

```ini
[audio]
source=clips/background.wav
volume=0.8
loop=1
```

**HAP 自带音频**（如果原始 HAP 文件含音轨）：无需 `[audio]` 段，程序自动启用。

---

## 4. 部署：开机自启 / 4K 直角 / 防休眠

### 4.1 开机自启三件套

`scripts/` 下三个 .bat：

```
scripts/
├── set-config.bat              # 配置 ExePath + ConfigPath（写注册表 HKCU\Software\happlayer）
├── install-autostart.bat       # 安装开机自启（注册表 Run\happlayer）
└── uninstall-autostart.bat     # 卸载开机自启
```

**部署流程**：

```bash
# 1. 拷贝 happlayer.exe + FFmpeg DLL + SDL2.dll 到目标目录
#    例：D:\Exhibit\happlayer\build\Release\

# 2. 拷贝视频素材
#    D:\Exhibit\happlayer\build\Release\clips\*.mov

# 3. 配置 layers.txt（同目录）

# 4. 双击 set-config.bat，选项：
#    [1] 修改 happlayer.exe 路径
#    [2] 修改 layers.txt 路径
#    [3] 安装开机自启
#    [4] 卸载开机自启
#    [5] 编辑 [audio] source
```

**注册表项**：

- 配置存：`HKCU\Software\happlayer\ExePath` / `ConfigPath`
- 自启项：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer` = `"<exe>" "<config>"`

### 4.2 4K 直角

Win11 默认给所有窗口加圆角 + 投影 → 拼接大屏时圆角处露白边。已内置绕过：

```ini
[window]
size=3840x2160
borderless=1   ← 必须
```

代码路径（`main.cpp` ~L1370）：用 `DwmSetWindowAttribute` 强制 `DWMWCP_DONOTROUND`。

### 4.3 全屏启动

```ini
[window]
fullscreen=1
```

启动后即占满目标显示器；`F` 键切换回窗口。

### 4.4 防休眠

**已内置**（无需配置）：`main.cpp` L1262 调用 `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)`。

- 阻止系统进入睡眠
- 阻止显示器关闭
- 进程退出时调 `ES_CONTINUOUS` 还原

> **注意**：如果用任务计划程序重启 happlayer，**不要**在 bat 里写 `timeout` 或 `ping -n` 等等待，要 `start "" happlayer.exe` 让其脱离父进程。

---

## 5. 排错清单

| 症状 | 诊断 | 解决 |
|------|------|------|
| 双击 .bat 窗口闪退 | 编码 / 行尾问题 | .bat 必须是 UTF-8 无 BOM + CRLF；中文 echo 必须 `chcp 65001 >nul` |
| 启动报 `glfwInit 失败` | 显卡驱动 / DLL 路径 | 重装显卡驱动；确保 FFmpeg DLL + SDL2.dll 在 exe 同目录 |
| 报 `显卡驱动不支持 S3TC 压缩纹理` | 老显卡 / 远程桌面 | 换 NVIDIA/AMD 独显；远程桌面需用 `/admin` 或 `mstsc` 直连 |
| 报 `[HAP-Q shader] 驱动不支持 GLSL` | OpenGL < 2.0 | 更新驱动；HAP Q 会自动降级为 RGB 显示（颜色可能偏） |
| 报 `[警告] PBO/Fence 不可用` | OpenGL < 3.2 | 自动降级同步上传，性能下降；可忽略或换驱动 |
| 黑屏但有声音 / 切场瞬间黑屏 | 多为解码线程还没预填满 | 等 5 秒 `preRoll`；或减小 `--max-fps` 给 CPU 让步 |
| 4K 视频严重卡顿 | CPU 软解 + RGBA 同步上传 | 加 `--max-fps 30`；若有 NVIDIA 卡确认 NVDEC 已启用（看启动日志 `[NVDEC]`） |
| HAP 视频显示"红/绿"偏色 | HAP Q 未走 shader | 检查显卡是否支持 GLSL；或重新用 `-format hap` 不用 `-format q_hap` |
| 视频首帧长时间加载 | 首层解码 + 磁盘 IO 慢 | 把视频放到 SSD；改 HAP 编码（GPU 直传无此问题） |
| 启动报 `找不到 layers.txt` | 路径错误 | 确认 `happlayer.exe` 与 `layers.txt` 同目录；或命令行第一个参数指定 |
| UDP 控制无响应 | 端口被占用 | `netstat -ano | findstr :7000`；用 `--port N` 换端口 |
| 开机自启无效 | 注册表没生效 | 检查 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer`；用 `--admin` 运行 `set-config.bat` |
| 显示 `[NVDEC] 未检测到 NVIDIA 驱动 DLL` | 无 NVIDIA 卡 / 驱动老 | 自动回退软解，无须操作；或装 NVIDIA 驱动 |
| 设了 `HAPPLAYER_USE_VULKAN=1` 后立刻崩 / 无日志 | GLFW 窗口带了 OpenGL context | 必须是当前 Release 构建；init 失败会直接退出（不能回退 GL） |
| Vulkan 只有第一帧、后面冻住 | 旧阶段 D 只上传首帧 | 换阶段 E 构建；日志应有 `[vulkan_renderer] init OK (阶段 E)` |

### 5.1 切场黑屏诊断

切场黑屏 = 队列切换时短暂无画面。常见原因：

1. **`preRoll` 没等够** — 4K 高码率下 ring buffer（12 帧）填满需要 >5 秒
   - 解决：保持 `preRoll` 5 秒（默认）
2. **`resetLayer` 重置了 `visible` 标志** — 已修复（保留 visible 不重置）
3. **下一队列的视频文件不存在 / 路径错** — 看启动日志
4. **GPU 纹理上传阻塞** — 加 `--max-fps 30` 给 GPU 让步

### 5.2 启动日志怎么看

```
检测到 2 台显示器：
  1: 2560x1600 @240Hz  虚拟坐标(0,0)  [主]
  2: 1920x1080 @60Hz  虚拟坐标(2560,0)
[NVDEC] 检测到 NVIDIA GPU + CUDA hwaccel，启用硬解    ← NVDEC 启用
[HAP-Q shader] 已编译 (program=3)                     ← HAP Q OK（OpenGL 路径）
[vulkan_renderer] init OK (阶段 E)                     ← 仅 HAPPLAYER_USE_VULKAN=1
[vulkan] 多层渲染管线已启用
>>> 队列 1/3 开始                                      ← 队列切换
[队列 1]                                              ← 当前队列层列表
    layer1: 3840x2160  30fps  loop=是  [HAP]
    layer2: 1920x1080  24fps  start=2s  loop=否  [H.264]
```

### 5.3 视频文件验证

```bash
# 验证编码 + 分辨率 + 帧率
deps\ffmpeg\bin\ffprobe.exe -show_streams -show_format clips\layer1.mov
```

期望输出片段：

```
codec_name=hap       ← HAP
width=3840
height=2160
r_frame_rate=30/1    ← 30fps
```

如果 `codec_name=h264` 而不是 `hap`，说明没用 HAP 编码，需要重新用 ffmpeg 转。

---

## 6. 性能调优

### 6.1 帧率自适应

**触发条件**：实测 FPS 持续低于 `targetFps * 60%`，每 5 秒把 `g_maxFps` 降 5fps（最低 15fps）。

**恢复条件**：实测 FPS 持续高于 `targetFps * 95%`，逐步升回初始值。

**手动覆盖**：用 `--max-fps N` 锁定上限。

**典型场景**：

| 场景 | 建议 |
|------|------|
| 30 层 HAP 4K 30fps | `--max-fps 30`（避免无效功耗） |
| 10 层混合 codec | 默认 60 + 自适应 |

### 6.2 资源选择

| 视频类型 | 推荐编码 | 原因 |
|----------|---------|------|
| 4K 高码率背景 | HAP（DXT1） | GPU 直传，零 CPU 解码 |
| 4K 半透明覆盖 | Hap Alpha（DXT5） | 同上 + alpha 通道 |
| 4K 文字 / 锐边 | Hap Q | YCoCg-DXT5 + shader，质量高 |
| 1080p 复杂动效 | H.264 + NVDEC | GPU 硬解，CPU 占用低 |
| 旧素材（h.264） | H.264 | 不重转直接用 |
| 透明 PNG 序列 | Animation (QTRLE) | FFmpeg 支持，CPU 解码重 |

### 6.3 性能基准（参考）

测试环境：Windows 11 + RTX 3060 + 2560x1600 @240Hz

| 配置 | 实测 FPS | CPU 占用 |
|------|---------|---------|
| 1 层 HAP 4K 30fps | 60 fps（cap） | <5% |
| 15 层 HAP 4K 30fps | 60 fps（cap） | 30% |
| 1 层 HAP + 1 层 H.264 4K | 32 fps（RGBA 同步上传瓶颈） | 60% |
| 1 层 HAP + 1 层 H.264 + NVDEC | 60 fps（cap） | 8% |
| 30 层 HAP 4K 30fps | 50-55 fps（自适应降 maxFps） | 50% |
| Vulkan 1 层 H.264 640×360 `--max-fps 30` | **30 fps 锁满**（2026-08-14 阶段 E） | 低于同素材 OpenGL ~22 |
| Vulkan 1 层 HAP 640×360 `--max-fps 30` | **30 fps 锁满** | 同 |

### 6.4 内存与显存占用

| 内容 | 4K 单层 | 备注 |
|------|---------|------|
| HAP（DXT1） | 8 MB / 帧 × 12 帧 = 96 MB | ring buffer |
| Hap Alpha（DXT5） | 16 MB / 帧 × 12 = 192 MB | DXT5 = 1 字节/像素 |
| Hap Q（YCoCg-DXT5） | 16 MB / 帧 × 12 = 192 MB | 同 DXT5 |
| H.264 RGBA8 | 32 MB / 帧 × 12 = 384 MB | YUV420 解码后转 RGBA |
| QTRLE RGBA | 32 MB / 帧 × 12 = 384 MB | 无压缩 |

**30 层 4K 全 HAP**：约 2.8 GB 显存（ring + texture 重复）。需 ≥ 4 GB 显存。

### 6.5 故障应急性能调优

如果现场卡顿，按以下顺序排查：

1. 看启动日志确认 NVDEC 是否启用
2. 加 `--max-fps 30`
3. 把视频转 HAP 编码（GPU 直传，CPU 占用降 90%）
4. 减少同时在播的层数
5. 拆分队列，让每场播放层数 ≤ 10
6. 有 Vulkan 依赖时试 `HAPPLAYER_USE_VULKAN=1`（失败会退出，先在测试机验证）

### 6.6 Vulkan 可选加速（阶段 E）

默认走 OpenGL。要切 Vulkan（需 `deps/vulkan/` 已编进 Release）：

```powershell
cd build\Release
$env:HAPPLAYER_USE_VULKAN='1'
.\happlayer.exe --bench 5 --max-fps 30
# HAP 素材示例（若有 layers_hap.txt）：
# .\happlayer.exe layers_hap.txt --bench 5 --max-fps 30
Remove-Item Env:HAPPLAYER_USE_VULKAN
```

成功日志应出现：

```
[vulkan_renderer] init OK (阶段 E)
[vulkan] 多层渲染管线已启用
```

注意：

- Vulkan 窗口是 `GLFW_NO_API`，**init 失败会直接退出**，不能回退 OpenGL。
- 不要和 OpenGL 共用同一个 HWND。
- 未做：窗口 resize、Vulkan+NVDEC 零拷贝、双缓冲 staging。
- 强制软解 H.264：`HAPPLAYER_NO_NVDEC=1`（与 Vulkan 开关独立）。

实测（2026-08-14，640×360，`--max-fps 30`）：OpenGL H.264 ~22 fps；Vulkan H.264 / HAP 均 30 fps 锁满。

---

## 附录 A：完整示例 layers.txt

```ini
# happlayer 配置示例 — 多队列循环播放
[window]
size=3840x2160
borderless=1
monitor=1
fullscreen=0

[audio]
source=clips/background.mp3
volume=0.6
loop=1

[queue]
# 队列 1：循环背景 + 入场动画
0|1|clips/bg_loop_hap.mov
2|0|clips/logo_in_hap.mov
3|0|clips/text_in_hap.mov

[queue]
# 队列 2：产品展示
0|1|clips/product_bg_hap.mov
1|0|clips/product_h264.mp4
3|0|clips/slogan_hap.mov
```

## 附录 B：完整 ffmpeg HAP 转码脚本

```bash
@echo off
chcp 65001 >nul
setlocal

if "%~1"=="" (
    echo 用法: transcode-hap.bat 输入.mp4 输出.mov
    exit /b 1
)

deps\ffmpeg\bin\ffmpeg.exe -y -i "%~1" ^
    -c:v hap -format hap -pix_fmt rgb ^
    -c:a aac -b:a 192k ^
    "%~2"
```

## 附录 C：故障速查卡

| 现象 | 90% 原因 | 一行命令 |
|------|---------|---------|
| 双击闪退 | .bat 编码 | 用 VS Code 转 UTF-8 无 BOM + CRLF |
| 视频没声音 | HAP 不带音 + 忘写 [audio] | 看启动日志 `[audio]` 段 |
| 报 `找不到 layers.txt` | 路径错 | 把 layers.txt 放到 exe 同目录 |
| 切场黑屏 | preRoll 没等够 | 等 5 秒（默认已够） |
| 4K 卡顿 | 软解 + 同步上传 | `--max-fps 30` + 转 HAP；或 `HAPPLAYER_USE_VULKAN=1` |
| NVDEC 没启用 | 无 NVIDIA 卡 | 装 NVIDIA 驱动；卡型不在白名单自动软解 |
| Vulkan 静默崩 | GL+VK 抢 HWND | 用阶段 E 构建；失败直接退出属预期 |

---

## 纠错记录（2026-09-16，据最终源码 + 实测补正）

> 遵循「纠错用增补不用覆盖」，原文保留；以下 3 处请以本表为准。
> 背景：本文部分内容停留在中间版本认知，与 8-20 最终代码及 README 矛盾。

### 勘误 1：ffmpeg `-format` 参数名写错（照抄会转码失败）

| | 文中（§3.1） | 正确（实跑 `ffmpeg -h encoder=hap` 验证） |
|---|---|---|
| Hap Alpha | `-format alpha_hap` | **`-format hap_alpha`** |
| Hap Q | `-format q_hap` | **`-format hap_q`** |
| Hap（无透明） | `-format hap` | `hap`（正确，取值 11/14/15） |

正确示例：`deps\ffmpeg\bin\ffmpeg.exe -i in.mov -c:v hap -format hap_alpha out.mov`

### 勘误 2：音频是「多源混音」，不是「内嵌优先、外挂被忽略」

- 文中 §2.3 称「用 HAP 内置音频时 `[audio]` 段会被忽略」——**已过时**（那是 v1.0 单源策略）。
- 最终版（v1.3）：每个带音轨的视频层各为一个音源（跟随层入场/退出/循环，
  音量取层行尾 `|音量%`），与 `[audio]` 外挂源**同时混音**，输出 48kHz stereo，
  主时钟 ±50ms 漂移校正。见 `src/audio.h` 顶部「多源混音」。

### 勘误 3：性能表过于乐观，以热机实测为准

- 文中 §6.3「15 层 4K 60fps cap / 30 层 4K 50-55fps」与最终实测矛盾，**勿据此做现场预期**。
- 最终实测（R7 6800H + RTX3060，见 README）：
  - 10 层 4K30 HAP Alpha 冷机 80~124 fps，**热机（满载约 10s 后降频）仅 6~13 fps**；
  - 15 层 4K30 冷机 25~28 fps，热机 3~9 fps；
  - 4 层 4K30 稳定 50+ fps；10 层 720p 无压力。
- **经验红线：稳定推荐 ≤4~6 层 4K 或 10+ 层 1080p**；H.264/QTRLE 只做 1080p 以下点缀层。
  验收数字必须在**目标展项主机上、跑满计划时长**测（冷机数字不算数）。

### 补充：当前交付的 exe 已与源码对齐

- 2026-09-16 核查曾发现 Release exe（8-17）旧于源码（8-20），当日已干净重建：
  编译 0 error、6/6 单测 PASS、bench 59fps。使用前如又改过源码，务必重新
  `cmake --build build --config Release`，不要直接跑旧 exe。