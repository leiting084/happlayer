# 部署指南

> 现场部署 happlayer 的完整流程：编译产物 → 拷贝依赖 → 写配置 → 拷贝视频 → 配置开机自启 → 验证 → 故障应急。

---

## 目录

1. [前置准备](#1-前置准备)
2. [标准部署流程](#2-标准部署流程)
3. [故障应急](#3-故障应急)
4. [备份策略](#4-备份策略)
5. [现场排查 SOP](#5-现场排查-sop)

---

## 1. 前置准备

### 1.1 现场环境

| 项 | 要求 |
|----|------|
| OS | Windows 10/11 64-bit |
| GPU | NVIDIA 优先（推荐；其他 OpenGL 1.5+ 也行） |
| 显卡驱动 | NVIDIA 521.xx+（启用 NVDEC 硬解所需） |
| 磁盘 | SSD 强烈推荐（视频文件 + happlayer.exe） |
| 权限 | 普通用户即可（注册表写 HKCU 不需 admin） |
| 网络 | 不需要（依赖全部打包在 exe 同目录） |

### 1.2 部署包清单

```
happlayer-deploy/
├── happlayer.exe                  ← 主程序
├── layers.txt                     ← 配置（部署时改）
├── avcodec-62.dll                 ← FFmpeg DLL（8 个）
├── avdevice-62.dll
├── avfilter-11.dll
├── avformat-62.dll
├── avutil-60.dll
├── swresample-6.dll
├── swscale-9.dll
├── ffmpeg.exe                     ← 可选（仅当现场要转码）
├── ffprobe.exe                    ← 可选（验证视频）
├── SDL2.dll
├── clips/                         ← 视频素材
│   ├── layer1_bottom.mov
│   ├── layer2_mid.mov
│   └── layer3_top.mov
└── scripts/                       ← 开机自启三件套
    ├── set-config.bat
    ├── install-autostart.bat
    └── uninstall-autostart.bat
```

**大小参考**：

| 组件 | 大小 |
|------|------|
| happlayer.exe + FFmpeg DLL + SDL2.dll | ~60 MB |
| 视频素材（10 个 HAP 4K 30s） | ~3 GB |
| 视频素材（30 个 HAP 4K 60s） | ~18 GB |

---

## 2. 标准部署流程

### Step 1：在开发机编译 Release 版

```bash
cd 项目根目录
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 验证
cd build\Release
.\happlayer.exe --bench 10            # 应能启动跑 10 秒
```

### Step 2：打包部署包

```bash
# 拷贝以下到部署目录 D:\Exhibit\happlayer\
xcopy /E /I build\Release D:\Exhibit\happlayer\Release
xcopy /E /I scripts D:\Exhibit\happlayer\scripts
```

**不要拷贝**：

- `*.exp` / `*.lib` / `*.pdb` — 调试符号
- `CMakeFiles/` / `*.cmake` / `CMakeCache.txt` — 构建中间产物
- `tests/` — 单元测试

### Step 3：写配置 `layers.txt`

```ini
[window]
size=3840x2160
borderless=1
monitor=1

[audio]
source=clips/bg.mp3
volume=0.8
loop=1

[queue]
0|1|clips/bg_loop.mov
2|0|clips/logo_in.mov
```

### Step 4：拷贝视频素材

```bash
# 验证每个视频编码
deps\ffmpeg\bin\ffprobe.exe -show_streams -show_format clips\bg_loop.mov | findstr codec_name width height r_frame_rate

# 期望：
#   codec_name=hap
#   width=3840
#   height=2160
#   r_frame_rate=30/1
```

### Step 5：现场测试

```bash
cd D:\Exhibit\happlayer\Release
.\happlayer.exe --bench 60
```

看启动日志：

- `[NVDEC] 检测到 NVIDIA GPU + CUDA hwaccel，启用硬解`
- `[HAP-Q shader] 已编译`
- `>>> 队列 1/N 开始`
- 平均 FPS ≈ 60

### Step 6：配置开机自启

```bash
cd D:\Exhibit\happlayer\scripts
set-config.bat
```

**菜单选项**：

```
[1] 修改 happlayer.exe 路径
[2] 修改 layers.txt 路径
[3] 安装开机自启
[4] 卸载开机自启
[5] 编辑 [audio] source
[6] 退出
```

按 `1` → 输入 `D:\Exhibit\happlayer\Release\happlayer.exe`
按 `2` → 输入 `D:\Exhibit\happlayer\Release\layers.txt`
按 `3` → 安装

**验证注册表**：

```powershell
# PowerShell
Get-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name happlayer
# 期望：
#   happlayer : "D:\Exhibit\happlayer\Release\happlayer.exe" "D:\Exhibit\happlayer\Release\layers.txt"
```

### Step 7：重启验证

```bash
# Windows 重启
shutdown /r /t 0

# 重启后应自动启动 happlayer.exe（看显示器）
```

**注意**：

- 重启前确认显示器已连接 + 电源已开
- 重启后如果显示器无画面，看 `C:\Users\<user>\AppData\Local\Temp\happlayer_stderr.log`（如果有 redirect）
- 当前实现无 log 落盘，需要手动 `start /min` + 重定向

---

## 3. 故障应急

### 3.1 进程崩溃 / 闪退

**症状**：现场显示器黑屏；任务管理器无 `happlayer.exe`。

**应急**：

1. **重启 happlayer**（手动）：
   ```bash
   cd D:\Exhibit\happlayer\Release
   .\happlayer.exe
   ```
   看 stderr（默认输出到控制台，崩溃时是黑窗闪退）

2. **加日志输出**（重新启动前）：
   ```bash
   .\happlayer.exe > happlayer.log 2>&1
   ```
   或在 bat 里：
   ```bat
   @echo off
   chcp 65001 >nul
   cd /d D:\Exhibit\happlayer\Release
   start "" happlayer.exe > happlayer.log 2>&1
   ```

3. **任务计划程序守护**（推荐永久方案）：
   - 触发器：开机 + 每 5 分钟检查一次
   - 操作：启动 `D:\Exhibit\happlayer\Release\happlayer.exe`
   - 条件：只在 "使用交流电源" 时启动
   - 设置：允许按需启动任务

   ```powershell
   $action = New-ScheduledTaskAction -Execute "D:\Exhibit\happlayer\Release\happlayer.exe"
   $trigger1 = New-ScheduledTaskTrigger -AtStartup
   $trigger2 = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 5) -RepetitionDuration (New-TimeSpan -Days 3650)
   Register-ScheduledTask -TaskName "happlayer_watchdog" -Action $action -Trigger $trigger1, $trigger2 -User "SYSTEM" -RunLevel Highest
   ```

### 3.2 视频卡顿

**症状**：画面不流畅；FPS 显示 < 30；CPU 占用 > 80%。

**应急**：

```bash
# 1. 降低帧率上限
.\happlayer.exe --max-fps 30

# 2. 减少同时在播层数（编辑 layers.txt）
# 3. 把 H.264 转 HAP（GPU 直传）
deps\ffmpeg\bin\ffmpeg.exe -y -i input.mp4 -c:v hap -format hap -pix_fmt rgb output.mov
```

**诊断**：

- 看启动日志 `[NVDEC]` 是否启用
- 看 `[自适应] 负载恢复` / `maxFps 降至` 字样（说明已自适应降帧）
- 看 `--bench N` 输出 FPS

### 3.3 音频卡顿 / 失步

**症状**：视频流畅但音频断断续续，或与视频不同步。

**应急**：

1. **检查 `[audio] source=` 文件路径**：
   ```bash
   # 验证音频文件可读
   ffprobe -show_streams "D:\Exhibit\happlayer\Release\clips\bg.mp3"
   ```

2. **改用 HAP 自带音频**（如有原带音频的 HAP 文件）：
   ```ini
   # 删掉 [audio] 段；HAP 内置音频会被自动启用
   ```

3. **减少视频解码压力**：
   ```bash
   .\happlayer.exe --max-fps 30
   ```

### 3.4 黑屏但有声音

**诊断流程**：

```
黑屏 + 有声音
   ↓
看显示器电源 + 信号线（确认显示器本身 OK）
   ↓
看显卡输出（Win+P 切显示模式）
   ↓
看任务管理器（happlayer.exe 是否在运行，CPU 是否 > 0）
   ↓
切场时机？
   ├─ 启动后立即黑 → [queue] 配置问题（路径错 / 文件不存在）
   ├─ 切场瞬间黑 → preRoll 没等够（已默认 5 秒，不应出现）
   └─ 随机黑 → 视频文件编码问题（用 ffprobe 验证）
```

**应急排查命令**：

```bash
# 1. 看启动日志（用绝对路径跑）
cd D:\Exhibit\happlayer\Release
.\happlayer.exe 2>&1 | tee run.log

# 2. 验证每个视频文件
deps\ffmpeg\bin\ffprobe.exe -show_streams clips\layer1.mov

# 3. 单独测一个层（最小化 layers.txt）
cat > test.txt <<EOF
[window]
size=640x480
borderless=0

[queue]
0|1|clips\layer1.mov
EOF

.\happlayer.exe test.txt --bench 30
```

### 3.5 黑屏 + 无声音（完全死锁）

**最严重**：happlayer.exe 进程在但 GPU 不响应。

**应急**：

1. **远程桌面连入**（mstsc 直连，不走 Optimus）：
   ```
   mstsc /v:<现场IP> /admin
   ```

2. **杀掉 + 重启**：
   ```bash
   taskkill /f /im happlayer.exe
   cd D:\Exhibit\happlayer\Release
   .\happlayer.exe
   ```

3. **重启机器**（最后手段）：
   ```bash
   shutdown /r /t 0
   ```

### 3.6 现场黑屏的"应急兜底"

如果现场无法远程、无法重启，准备**U 盘启动盘**：

```
USB-drive/
├── happlayer.exe
├── 8 个 FFmpeg DLL
├── SDL2.dll
├── layers.txt
└── clips/                  ← 最小素材
```

插入后用管理员 cmd 运行：

```bat
X:\happlayer.exe --bench 60 --fullscreen
```

---

## 4. 备份策略

### 4.1 配置备份

`layers.txt` 是核心配置，必须**多版本管理**：

```bash
# 在开发机用 git 管理（推荐）
cd 项目根目录
git add layers.txt docs/
git commit - "layers 现场部署 v1.0"

# 或用 OneDrive / NAS 自动同步
robocopy D:\Exhibit\happlayer\Release\layers.txt \\nas\backup\ /Z
```

### 4.2 视频素材备份

视频素材大（GB 级），推荐：

- **LFS（Git Large File Storage）**：开发机用 git-lfs 跟踪 .mov 文件
- **NAS 镜像**：rsync / robocopy 每天定时同步
- **离线冷备**：每月刻盘（蓝光 50GB/盘）

### 4.3 完整目录快照

```bash
# 开发机每周一次完整备份（不含 build/ 和 .git/）
tar -czf happlayer-$(date +%Y%m%d).tar.gz \
    --exclude='build' \
    --exclude='.git' \
    --exclude='*.log' \
    D:\Exhibit\happlayer\

# 上传到云端
rclone copy happlayer-20260813.tar.gz remote:backup/
```

### 4.4 版本回滚 SOP

如果新版本出问题：

1. **停掉当前进程**：
   ```bash
   taskkill /f /im happlayer.exe
   ```

2. **从备份恢复**：
   ```bash
   # 备份当前到 old/
   robocopy D:\Exhibit\happlayer\Release D:\Exhibit\happlayer\old /E
   # 恢复上个版本
   robocopy \\nas\backup\happlayer-20260813\Release D:\Exhibit\happlayer\Release /E
   ```

3. **重启**：
   ```bash
   cd D:\Exhibit\happlayer\Release
   .\happlayer.exe
   ```

---

## 5. 现场排查 SOP

### 5.1 标准排查流程（10 分钟内定位）

```
[症状]
   ↓
[1] 看显示器 + 信号 → 显示器问题？（换一根 DP 线）
   ↓ OK
[2] 看任务管理器 → happlayer.exe 进程在吗？
   ├─ 不在 → 进程崩了，跳到 [A]
   └─ 在但 CPU = 0 → 死锁，跳到 [B]
   ↓ CPU > 0
[3] 看启动日志（如果 redirect 到文件）
   ├─ [NVDEC] 未检测到 → 检查显卡驱动
   ├─ [HAP-Q shader] 失败 → GLSL 不支持
   ├─ layers.txt 解析报错 → 配置问题
   └─ 找不到视频文件 → 路径错
   ↓ OK
[4] 单层最小化测试（test.txt）
   ├─ 成功 → 多层配置问题（队列顺序 / preRoll）
   └─ 失败 → 视频文件问题（用 ffprobe 验证）
```

### 5.2 [A] 进程崩溃排查

```bash
# 看 Windows 事件日志
eventvwr.msc
# → Windows 日志 → 应用程序 → 找最近 ERROR 级别的 happlayer 记录

# 启动 + 输出到文件
cd D:\Exhibit\happlayer\Release
.\happlayer.exe 2>&1 > run.log
# 崩溃时最后几行是关键
```

### 5.3 [B] 死锁排查

```bash
# 用 procdump 看 进程栈（需安装 sysinternals）
procdump -ma happlayer.exe dump.dmp

# 或用 WinDbg 看线程栈
# （高级，需要 Windows SDK）
```

常见死锁原因：

- `resetLayer` 没正确释放资源导致下次切场时 hang
- `preRoll` 死等 ring 填满（视频文件损坏 / 0 长度）
- 多个层同时 `cv.wait` 在同一 mutex 上 + 顺序错

### 5.4 远程诊断（人在外面）

```powershell
# 启用远程桌面（如未开）
# 现场 Windows 设置 → 远程桌面 → 启用

# 连接
mstsc /v:<现场IP> /admin

# 查看进程
Get-Process happlayer

# 查看注册表
Get-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name happlayer

# 查看视频文件
Get-ChildItem D:\Exhibit\happlayer\Release\clips\

# 重启 happlayer
Stop-Process -Name happlayer -Force
Start-Process D:\Exhibit\happlayer\Release\happlayer.exe
```

### 5.5 现场应急联系卡

```
项目名：        happlayer 多层视频播放器
版本：          v1.0（2026-08-13）
关键路径：      D:\Exhibit\happlayer\Release\
配置：          D:\Exhibit\happlayer\Release\layers.txt
自启注册表：    HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer
紧急重启：      taskkill /f /im happlayer.exe & cd D:\Exhibit\happlayer\Release & happlayer.exe
紧急停机：      taskkill /f /im happlayer.exe
完整重装：      robocopy \\nas\backup\happlayer-20260813\Release D:\Exhibit\happlayer\Release /E
```

---

## 附录 A：部署检查清单

```
□ 编译 Release 版（build\Release\happlayer.exe）
□ 验证启动（--bench 10 通过）
□ 写 layers.txt（含 [window] [audio] [queue]）
□ 拷贝 8 个 FFmpeg DLL + SDL2.dll
□ 拷贝视频素材到 clips/
□ 拷贝 scripts/ 三件套
□ 现场跑 --bench 60（验证 60fps）
□ 配置开机自启（set-config.bat）
□ 注册表验证（Get-ItemProperty）
□ 重启验证
□ 现场录像 / 截图存档
```

## 附录 B：常见部署错误

| 错误 | 原因 | 解决 |
|------|------|------|
| 现场启动报 `找不到 avcodec-62.dll` | FFmpeg DLL 没拷 | 拷 `deps\ffmpeg\bin\*.dll` 到 exe 同目录 |
| 现场启动报 `找不到 SDL2.dll` | SDL2.dll 没拷 | 拷 `deps\sdl2\lib\x64\SDL2.dll` |
| 现场启动报 `找不到 layers.txt` | 路径配置错 | 用绝对路径；或把 layers.txt 放 exe 同目录 |
| 现场 4K 显示比例错 | size 错 | 设 `size=3840x2160` |
| 现场拼接大屏露白边 | borderless=0 | 设 `borderless=1` |
| 现场无声音 | 忘写 `[audio]` 或 HAP 不带音 | 加 `[audio] source=` 段 |
| 自启失效 | 注册表项被清理软件清掉 | 重新跑 `set-config.bat` 选项 3 |
| 自启启动但显示器黑 | Win11 圆角 + 无 borderless | `[window] borderless=1` |
| 自启启动但视频卡 | preRoll 没等够 | 默认 5 秒应够；不够改源码 `preRoll(q, 8.0)` |