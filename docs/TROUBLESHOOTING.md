# 排错清单（TROUBLESHOOTING）

> happlayer 现场 / 开发期常见问题 + 诊断命令 + 解决步骤。
> 按"症状 → 根因 → 解决"组织，方便快速定位。

---

## 目录

- [A. 启动问题](#a-启动问题)
- [B. 视频显示问题](#b-视频显示问题)
- [C. 性能问题](#c-性能问题)
- [D. 音频问题](#d-音频问题)
- [E. 切场 / 队列问题](#e-切场--队列问题)
- [F. 自启 / 部署问题](#f-自启--部署问题)
- [G. UDP 控制问题](#g-udp-控制问题)
- [H. 编译问题](#h-编译问题)
- [I. Vulkan 可选路径](#i-vulkan-可选路径)

---

## A. 启动问题

### A1. 双击 exe 闪退

**症状**：双击 `happlayer.exe` 窗口闪一下就没了。

**诊断**：

```bash
# 用 cmd 启动看 stderr
cd D:\Exhibit\happlayer\Release
happlayer.exe
# 不闪退，能看到错误信息
```

**常见根因 + 解决**：

| 根因 | 解决 |
|------|------|
| 找不到 `avcodec-62.dll` 等 FFmpeg DLL | 拷贝 `deps\ffmpeg\bin\*.dll` 到 exe 同目录 |
| 找不到 `SDL2.dll` | 拷贝 `deps\sdl2\lib\x64\SDL2.dll` |
| 找不到 `layers.txt` | 第一个参数指定，或放 exe 同目录 |
| 路径含中文 + GBK 编码解析失败 | 用纯英文路径；或 `chcp 65001 >nul` 再启动 |
| 显卡驱动崩溃 | 重启机器；更新 NVIDIA 驱动 |

### A2. 启动报 `glfwInit 失败`

**症状**：`std::cerr << "glfwInit 失败\n"`

**根因**：OpenGL 子系统初始化失败，多为驱动问题。

**解决**：

1. 重启机器（驱动状态被重置）
2. 升级显卡驱动
3. 检查远程桌面 / Optimus 设置（用独显跑）

### A3. 启动报 `显卡驱动不支持 S3TC 压缩纹理`

**症状**：

```
std::ERR << "显卡驱动不支持 S3TC 压缩纹理\n"; return 1;
```

**根因**：HAP 视频用 DXT 压缩纹理直传 GPU，显卡必须支持 S3TC。

**解决**：

- NVIDIA / AMD 独显：**支持**，更新驱动即可
- Intel 集显：HD 4000+ 支持
- 远程桌面：可能不支持，物理直连或用 `/admin`
- 老显卡（NVIDIA 8 系列以前）：不支持，换硬件

### A4. 启动报 `[HAP-Q shader] 驱动不支持 GLSL`

**症状**：

```
[HAP-Q shader] 驱动不支持 GLSL
```

**根因**：HAP Q 需要 OpenGL 2.0+ GLSL 编译支持。

**解决**：

- 更新显卡驱动
- 若只有 HAP Q 素材受影响，会自动降级为 RGB（颜色偏）；HAP（DXT1）/ Hap Alpha（DXT5）不受影响
- 用 `-format hap` 重新编码（不用 `-format q_hap`）

### A5. 启动报 `[警告] PBO/Fence 不可用`

**症状**：

```
[警告] 显卡驱动不支持 PBO/Fence，P0.1 PBO 异步上传将自动降级为同步上传
```

**根因**：OpenGL 1.5/3.2 扩展缺失，4K 15 层会卡。

**解决**：

- 自动降级同步上传，性能下降但仍能跑
- 长期方案：升级驱动；4K 多层场景建议换 N 卡

### A6. 启动报 `未检测到 NVIDIA 驱动 DLL`

**症状**：

```
[NVDEC] 未检测到 NVIDIA 驱动 DLL，走软解
```

**根因**：现场机器没装 NVIDIA 驱动，或装了但驱动目录不在 PATH。

**解决**：

- 自动回退软解，**无须操作**
- 若有 N 卡但想用 NVDEC：
  1. 装 NVIDIA 驱动（带 CUDA）
  2. 验证 `nvidia-smi` 能识别卡
  3. 验证 FFmpeg 编译时带 `h264_cuvid`：`ffmpeg -hide_banner -codecs | findstr h264_cuvid`

---

## B. 视频显示问题

### B1. 黑屏

**症状**：显示器亮但完全黑，看不到任何层。

**诊断**：

```bash
# 1. 看进程在不在
tasklist | findstr happlayer

# 2. 看启动日志（用 cmd 启动）
happlayer.exe 2>&1 | tee run.log

# 3. 单层最小化测试
# 写一个 test.txt，只含 1 层
[window]
size=640x480
borderless=0
[queue]
0|1|clips\layer1.mov

# 用此文件启动
happlayer.exe test.txt --bench 30
```

**根因 + 解决**：

| 根因 | 解决 |
|------|------|
| 视频文件不存在 | 检查路径；用 ffprobe 验证 |
| 视频文件编码异常 | 用 ffprobe 看 `codec_name` |
| `layers.txt` 解析失败 | 看 stderr 报哪行；用纯文本编辑器（VS Code）保存 |
| `borderless=0` + Win11 → 窗口在屏幕外 | 设 `borderless=1` 或 `fullscreen=1` |
| `--pos` 超出屏幕 | 改 `pos=0,0` |
| 设了 `HAPPLAYER_USE_VULKAN=1` 但无 GPU / init 失败 | 进程直接退出（NO_API 窗口不能回退 GL）；去掉环境变量走 OpenGL |
| Vulkan 只有第一帧、后面冻住 | 旧阶段 D；换阶段 E 构建，日志需有 `init OK (阶段 E)` |

### B2. 切场瞬间黑屏（已修复，但仍偶现）

**症状**：从队列 1 切到队列 2 时黑 0.5-1 秒。

**根因**：`preRoll` 没等够；或下一队列视频文件损坏。

**解决**：

```bash
# 1. 看 stderr "preRoll" 字样
#    应看到 "preRoll(q, 5.0s)" + 实际等待时间
#    若实际等待 < 1 秒 → ring buffer 早就填满了，不会黑
#    若实际等待 > 5 秒 → 视频解码慢，需要更多 preRoll

# 2. 验证下一队列视频
ffprobe -show_streams clips\next_queue_layer.mov
```

### B3. HAP Q 颜色偏红/偏绿

**症状**：HAP Q 视频颜色明显不对。

**根因**：DXT5 数据被直接当 RGB 上传，YCoCg 颜色空间被误读。

**解决**：

- 启动日志确认 `[HAP-Q shader] 已编译`
- 若报 `驱动不支持 GLSL`：更新驱动
- 用 `-format hap` 重新编码（不用 `-format q_hap`），HAP 不带色彩空间转换

### B4. 视频显示位置错 / 比例不对

**症状**：视频在错误的位置，或被拉宽/压扁。

**解决**：

```ini
[window]
size=1920x1080    # 改成视频原始分辨率
scale=100         # 不缩放
```

### B5. 显示不全（被显示器边缘裁切）

**症状**：4K 视频在 4K 显示器上看着缺边。

**根因**：`pos` 偏移超出显示器范围。

**解决**：

```ini
[window]
size=3840x2160
pos=0,0       # 改回左上角
borderless=1
```

### B6. 跨屏拼接有缝隙

**症状**：双 1920x1080 拼接显示 3840x1080 内容，中间有黑边。

**根因**：`size` 与显示器排列不对应。

**解决**：

1. Windows 显示设置 → 多显示器 → 排列成 3840x1080 逻辑布局
2. `happlayer` 的 `[window] size=3840x1080 borderless=1`（**必须无边框**才能跨屏拼接）

### B7. Win11 圆角 + 白边

**症状**：4K 直角显示被 Win11 加了圆角，露出白边。

**解决**：

```ini
[window]
borderless=1   ← 必须
```

`main.cpp` ~L1370 已内置 `DwmSetWindowAttribute(DWMWCP_DONOTROUND)`，设置 `borderless=1` 即生效。

---

## C. 性能问题

### C1. 4K 视频卡顿（FPS < 30）

**诊断**：

```bash
# 看启动日志
happlayer.exe 2>&1 | tee run.log
# 关注 [NVDEC] [自适应] 提示
```

**根因 + 解决**：

| 根因 | 解决 |
|------|------|
| H.264 软解 + RGBA 同步上传 | 加 `--max-fps 30`；装 NVIDIA 驱动开 NVDEC |
| H.264 多层并发 CPU 100% | 转 HAP 编码（GPU 直传，CPU 几乎不占） |
| 太多层同时在播 | 减少 layers.txt 中同队列层数（≤ 10） |
| 磁盘 IO 慢（视频在 HDD） | 视频放 SSD |
| 显卡驱动老 | 升级 NVIDIA 驱动到最新版 |
| 自适应已降 maxFps 到 15 | 减层数 / 升驱动 |

### C2. 自适应降帧后不能回升

**症状**：`[自适应] 负载恢复` 没出现，maxFps 一直停在 15。

**诊断**：

```bash
# 看 FPS 趋势
happlayer.exe --bench 60 2>&1 | findstr "FPS\|fps"
```

**根因**：负载没真正减轻（CPU/GPU 仍 100%）。

**解决**：

- 减层数
- 转 HAP 编码
- 升级硬件

### C3. 启动后前 5 秒卡

**症状**：启动后 5 秒内 FPS = 0，然后恢复正常。

**根因**：`preRoll` 5 秒（默认）= 等待 ring buffer 填满。

**解决**：

- 5 秒是设计预期，**不是 bug**
- 想更快启动可改 `main.cpp` `preRoll(q, 5.0)` → `preRoll(q, 3.0)`（风险：切场可能瞬黑）

### C4. 内存 / 显存爆

**症状**：进程占用 > 4 GB；显卡驱动崩。

**根因**：30 层 HAP Alpha 4K = 30 × 192 MB = 5.7 GB（已超 4 GB 显存）。

**解决**：

- 减少同播层数（拆队列）
- 用 HAP（DXT1，无 alpha）替代 Hap Alpha
- 升级显卡 ≥ 8 GB 显存

---

## D. 音频问题

### D1. 无声音

**诊断**：

```bash
# 1. 看启动日志 [audio] 段
happlayer.exe 2>&1 | findstr "audio\|HAP 自带"

# 期望：
#   [audio] HAP 不带音频且未配置 [audio] source，跳过音频初始化
#   或 HAP 自带音频已启用
#   或 [audio] source=... 已加载
```

**根因 + 解决**：

| 根因 | 解决 |
|------|------|
| HAP 不带音 + 忘写 `[audio]` | 加 `[audio] source=clips\bg.mp3` |
| 路径错 | 用绝对路径测试 |
| 文件不是 mp3/wav | 用 ffprobe 看 `codec_name`，转成 aac |
| 系统音量 0 | 调 Win 音量 |
| Windows 静音 | 看屏幕右下角扬声器图标 |

### D2. 音频卡顿

**症状**：音频断断续续，或像加速播放。

**根因**：SDL 缓冲耗尽（解码线程来不及）。

**解决**：

```bash
# 1. 减视频压力
happlayer.exe --max-fps 30

# 2. 用 HAP 自带音频（HAP 内嵌音轨解码更稳）

# 3. 视频转 HAP（CPU 占用降 → 解码音频有富余）
```

### D3. 音视频不同步

**症状**：音画差 1-2 秒。

**根因**：视频时钟 vs 音频时钟漂移。

**解决**：

- 程序每帧校正 ±50ms（小漂移自动修）
- 大漂移通常因视频重 IO 阻塞 → 减层数 / 换 SSD
- 用 HAP 自带音频替代外挂 wav（共享 fmt 上下文更稳）

### D4. 静音键 M 无效

**根因**：音频未初始化。

**解决**：见 D1 修复音频。

---

## E. 切场 / 队列问题

### E1. 队列不切换

**症状**：第一队播完后不切到第二队，一直重复第一队。

**根因**：第一队有循环层（`loop=1`），永远不会"播完"。

**解决**：

```ini
[queue]
# 改：底层循环 + 顶层不循环
0|1|clips\bg.mov        # 永远循环
5|0|clips\text.mov      # 5 秒入场，播完退出
# 当 text.mov 播完 → 队列结束 → 切下一队列
```

### E2. 队列切换错乱

**症状**：不按配置的顺序切场。

**诊断**：检查 `[queue]` 段顺序。

```ini
[queue]    ← 1
...
[queue]    ← 2
...
```

### E3. 切场后视频不显示

**症状**：第一队正常，切到第二队后画面没了。

**诊断**：

```bash
# 单独测第二队视频
ffprobe -show_streams clips\second_layer.mov

# 看编码 + 分辨率 + 时长
```

**根因 + 解决**：

| 根因 | 解决 |
|------|------|
| 视频文件损坏 | 重新转码 |
| 路径错（错别字） | 用绝对路径 |
| 视频时长 < 入场秒 | 调整 `start=Ns` < 视频时长 |
| 视频编码不支持（BC7） | 重新用 `libx264` 或 `hap` 编码 |

### E4. R 键重播当前队列无效

**症状**：按 R 没反应。

**诊断**：

```bash
# 1. 确认键盘事件
# 启动日志会显示 "key=R" 字样（如果有）

# 2. 看 stderr
happlayer.exe 2>&1 | findstr R
```

**根因**：键盘事件被窗口外接收（如 RDP 焦点）；或 `glfwSetKeyCallback` 未注册（编译错误）。

**解决**：

- 物理键盘而非远程桌面
- 重新编译

---

## F. 自启 / 部署问题

### F1. 自启不生效

**诊断**：

```powershell
Get-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name happlayer
```

**根因 + 解决**：

| 根因 | 解决 |
|------|------|
| 注册表项没写 | 跑 `set-config.bat` 选项 3 |
| 路径被清理软件清掉 | 重新跑 `set-config.bat` 选项 3 |
| exe 路径变了 | 选项 1 改 ExePath |
| layers.txt 路径变了 | 选项 2 改 ConfigPath |
| Win11 旧版"快速设置"清掉了 | 检查启动文件夹是否有快捷方式 |

### F2. 自启启动后显示器黑

**根因**：Win11 圆角 + 无 borderless。

**解决**：

```ini
[window]
borderless=1
```

### F3. 自启启动但立刻退出

**根因**：启动时间 < 5 秒就退出 = `preRoll` 失败或视频文件 0 长度。

**解决**：

```bash
# 手动启动看错误
cd D:\Exhibit\happlayer\Release
happlayer.exe
```

### F4. 现场找不到 happlayer.exe

**根因**：U 盘未插入 / 目录被删 / 网络盘掉线。

**应急**：见 `DEPLOYMENT.md` §3.6。

---

## G. UDP 控制问题

### G1. UDP 端口不可达

**症状**：`echo play | nc -u 127.0.0.1 7000` 无响应。

**诊断**：

```bash
# 1. 看端口监听
netstat -ano | findstr :7000

# 2. 看启动日志 [UDP]
happlayer.exe 2>&1 | findstr UDP
# 期望：UDP 控制端口 7000：...
```

**根因 + 解决**：

| 根因 | 解决 |
|------|------|
| 端口被占用 | `--port N` 换端口 |
| happlayer 启动时 `--port 0` | 改回 `--port 7000` |
| 防火墙拦截 | 关 Windows Defender 防火墙或加例外 |
| 命令错误（拼写） | `play` 不带空格、不带换行 |

### G2. UDP 收到但无响应

**症状**：`nc -u` 显示发送成功，但 happlayer 日志没变化。

**根因**：命令格式错误。

**解决**：

```bash
# 命令必须是以下之一（小写）：
play / pause / toggle / restart / next / prev / queue N / status / quit

# queue 必须带空格和数字
echo "queue 3" | nc -u 127.0.0.1 7000   # 正确
echo "queue3"  | nc -u 127.0.0.1 7000   # 错误）
```

---

## H. 编译问题

### H1. CMake 找不到 FFmpeg

**症状**：`Could not find ffmpeg`。

**解决**：

- 确认 `deps\ffmpeg\include\libavformat\avformat.h` 存在
- 确认 `deps\ffmpeg\lib\avformat.lib` 存在
- 确认 `CMakeLists.txt` 第 21 行 `set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/deps/ffmpeg)`

### H2. 编译报 `windows.h` 与 `winsock2.h` 冲突

**症状**：

```
winsock.h has already been included
```

**根因**：`<windows.h>` 在 `<winsock2.h>` 之前 include。

**解决**：

```cpp
// 必须按这个顺序
#include <winsock2.h>    ← 先
#include <ws2tcpip.h>
#include <GLFW/glfw3.h>
#include <windows.h>     ← 后
```

`main.cpp` L24-35 已按此顺序。

### H3. 链接错误 `unresolved external symbol __imp_avcodec_*`

**根因**：FFmpeg 链接库缺失。

**解决**：

```cmake
target_link_libraries(happlayer PRIVATE
    avformat avcodec avutil swresample swscale   ← 必须全部
    ...
)
```

### H4. 编译报 `GLsync` 未定义

**症状**：`unknown type name 'GLsync'`。

**根因**：MSVC `gl.h` 是 OpenGL 1.1，无 GLsync。

**解决**：

```cpp
typedef struct __GLsync* GLsync;
```

已写在 `main.cpp` L106。

### H5. 编译报 `GLuint64` 未定义

**根因**：同上，MSVC `gl.h` 缺失。

**解决**：

```cpp
#ifndef GLuint64
typedef unsigned __int64 GLuint64;
#endif
```

已写在 `main.cpp` L107-109。

### H6. 编译报 `'M_PI' was not declared`

**根因**：MSVC `<cmath>` 不默认暴露 M_PI。

**解决**：

```cpp
#define _USE_MATH_DEFINES
#include <cmath>
```

或在 `main.cpp` 加 `#define M_PI 3.14159265358979323846`。

---

## I. Vulkan 可选路径

> 默认 OpenGL。仅当 `HAPPLAYER_USE_VULKAN=1` 且 Release 编进了 `HAPPLAYER_HAS_VULKAN` 时走本节。

### I1. 启动立刻崩，几乎没有 Vulkan 日志（Access Violation `0xC0000005`）

**症状**：日志停在「检测到 N 台显示器」，连 `init 失败` 都没有。

**根因**：GLFW 窗口已创建 OpenGL context，再 `vkCreateWin32SurfaceKHR` 挂同一 HWND。

**解决**：必须用阶段 E 构建——`glfwCreateWindow` **之前** `GLFW_CLIENT_API = GLFW_NO_API`。不要 `glfwMakeContextCurrent` / `gl*`。init 失败会退出，这是预期。

### I2. `[vulkan] vkCreateGraphicsPipelines` 返回 `-13`（`VK_ERROR_UNKNOWN`）

**症状**：shader module 创建成功，pipeline 失败。

**根因**：嵌入的 glslang SPIR-V 对 module 合法、对 pipeline 接口不合法（如未启用 feature 的 ClipDistance）。

**解决**：用当前源码里的手写最小 vert（`kSimpleVertSpirv`）+ RGBA / YCoCg frag。`vkCreateShaderModule` 成功不能当验收。

### I3. 画面停在第一帧

**根因**：阶段 D 只上传首帧，后续 `updateTexture` 未进 command buffer。

**解决**：阶段 E：CPU `memcpy` 到持久 staging，draw 里 `recordCopyToImage` 放在 render pass 之前。

### I4. 第三帧左右花屏 / 校验层报 descriptor pool 耗尽

**根因**：每帧 `vkAllocateDescriptorSets`，`maxSets` 太小。

**解决**：启动时预分配 `kMaxLayers × framesInFlight` 套。

### I5. 链接 `LNK1104` 无法打开 `happlayer.exe`

**根因**：上次崩溃的进程仍占用 exe。

**解决**：结束该 PID 后再 `cmake --build build --config Release --target happlayer`。不要用 `taskkill /im happlayer.exe` 全量杀（见全局规则进程清理用 PID）。

### I6. 没有 `VK_LAYER_KHRONOS_validation`

本机没装 SDK 校验层时不会打印该层。用「最小 shader 替换法」隔离，不要空等 layer。

---

## 附录 A：诊断速查卡

| 现象 | 第一步 | 命令 |
|------|--------|------|
| 启动闪退 | 看 stderr | `happlayer.exe 2>&1 \| tee run.log` |
| 黑屏 | 验证视频 | `ffprobe clips\layer1.mov` |
| 卡顿 | 看 FPS | `happlayer.exe --bench 60` |
| 无声音 | 看启动日志 | `happlayer.exe 2>&1 \| findstr audio` |
| 自启失效 | 看注册表 | `Get-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name happlayer` |
| UDP 无响应 | 看端口 | `netstat -ano \| findstr :7000` |
| 切场错乱 | 看 layers.txt | 文件位置：exe 同目录 |
| Vulkan 静默崩 | 是否 GLFW_NO_API | 阶段 E 构建；去掉 `HAPPLAYER_USE_VULKAN` 走 GL |
| pipeline -13 | SPIR-V 接口 | 用手写最小 vert/frag，勿信 module 成功 |

## 附录 B：日志保留建议

调试时加日志输出：

```bat
@echo off
chcp 65001 >nul
cd /d D:\Exhibit\happlayer\Release
start "" happlayer.exe > happlayer_%date:~0,4%%date:~5,2%%date:~8,2%.log 2>&1
```

每日一个 log 文件，方便现场回溯。