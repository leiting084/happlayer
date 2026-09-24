# happlayer — 多层透明视频展项播放器

Windows 展项用多层透明视频播放器。HAP 编码（DXT 压缩纹理直传 GPU，CPU 占用极低），
多队列场次编排，每层独立定时入场/循环/播完退出/位置/缩放/音量，场间淡入淡出，
UDP + OSC 远程控制，多源音频混音，多显示器/无边框/跨屏拼接，配置热重载。

- 渲染：GLFW + OpenGL（PBO 异步纹理上传）；实验性 Vulkan 路径（`HAPPLAYER_USE_VULKAN=1`）
- 解封装：FFmpeg（gyan full_build-shared，MSVC 预编译包）
- 解压：snappy（源码构建）；音频：SDL2 多源混音
- 构建：CMake + VS2022（命令行全自动，不需要开 IDE）

## 快速开始

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd build/Release && happlayer.exe
```

`build/Release/` 是自包含播放包，整个文件夹拷到展项主机即可。
改节目只改旁边的 `layers.txt`——**保存即热重载**（队列/图层/音频/scale/fade 立即生效；
窗口几何 size/pos/monitor/fullscreen/borderless 仍需重启进程）。
新配置无可用队列时忽略本次变更、继续播旧配置（现场改错字不黑屏）。

## 第三方依赖（`deps/`，不入库）

`deps/` 不随仓库分发，构建前按 CMake 期望的目录结构放好：

| 目录 | 内容 | 获取方式 |
|---|---|---|
| `deps/ffmpeg/` | gyan.dev full_build-shared 解压（含 include/ lib/ bin/） | [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) 下载 `ffmpeg-release-full-shared.7z`，解压重命名为 `ffmpeg`（解 7z 用系统 `tar.exe`） |
| `deps/glfw/` | GLFW 预编译包（include/ lib-vc2022/） | [glfw.org](https://www.glfw.org/download) Windows 64-bit binaries |
| `deps/sdl2/` | SDL2 VC 开发包（include/ lib/x64/ cmake/） | [libsdl-org/SDL releases](https://github.com/libsdl-org/SDL/releases) |
| `deps/snappy/` | snappy 源码（随项目一起 CMake 构建） | clone [google/snappy](https://github.com/google/snappy) |
| `deps/vulkan/`（可选） | Vulkan header + `vulkan-1.lib`（include/ lib/） | 从 LunarG SDK 拷贝；缺省则跳过实验性 Vulkan 路径 |

测试素材不入库：`make_test_clips.bat` 生成 720p 测试片到 `clips/`；
4K 压测素材需自行生成到 `C:/hap4k_clips/`（加噪素材，见 TEST_PLAN §6.3）。

## layers.txt 配置

```ini
[window]
size=3840x2160     # 窗口分辨率；跨屏拼接写总尺寸（双1080p横屏: 3840x1080）
pos=0,0            # 相对「目标显示器」左上角的偏移
monitor=1          # 从第几台显示器开始（1 起，启动日志列编号）
scale=100          # 全局内容缩放百分比（10~400，窗口中心缩放）
fade=0.5           # 场间淡入淡出时长（秒，0=硬切）
fullscreen=0       # 1 = 占满目标显示器的真全屏（size/pos 失效）
borderless=1       # 1 = 无边框（跨屏必须；Win11 下已强制直角）

[audio]            # 可选，外挂音频源（与所有内嵌音频层混音）
source=clips/bgm.mp3
volume=0.8
loop=1

[preload]          # 可选，短素材预加载到内存（仅非 HAP 层：H.264/QTRLE）
enabled=1
max_duration=30

[queue]            # 每个 [queue] 一场；层从下往上叠（第一行最底层）
# 格式: 开始秒|循环|路径[|x%|y%|宽%|音量%]
0|1|clips/bg.mov              # 循环背景层（contain 居中，默认布局）
2|0|clips/mid.mov             # 2 秒入场，播完自动退出
5|0|clips/top.mov
0|1|clips/logo.mov|85|82|12   # 自定义位置：左上角在窗口 (85%,82%)，宽占 12%（高按比例）
0|1|clips/speech.mov|50|30|40|80  # 宽 40% 居中上、该层内嵌音频音量 80%

[queue]            # 第二场……
0|1|clips/other.mov
```

**队列规则**
- 层按队列时钟各自入场；不循环层播完退出；**所有不循环层播完 → 切下一队列**，最后回到第一场
- 循环层不决定队列结束；每场至少一层不循环，否则永不切场（启动警告）
- **单队列 + 全循环 = 永不切场，不会黑场**；`fade>0` 时切场走 黑场淡出→切换→淡入
- 自定义矩形 `|x%|y%|宽%`：x,y 是矩形**左上角**相对窗口宽/高的百分比；音量% 只对该层内嵌音频生效
- 相对路径以配置文件所在目录为基准；支持绝对路径

## 命令行

```
happlayer [config] [--bench 秒] [--port 端口] [--size 宽x高] [--pos x,y]
          [--monitor N] [--scale 百分比] [--fade 秒] [--fullscreen] [--borderless]
          [--max-fps N] [--json] [--log-level error|warn|info|debug]
```
- 窗口参数覆盖 `[window]` 段；`--port 0` 关 UDP/OSC（默认 7000）
- `--bench N`：跑 N 秒打印统计后退出（压测用）；`--json` 输出 JSON 报告
- `--max-fps`：渲染帧率上限（默认 60，自适应下调，下限 30）

**键盘**：空格=暂停  R=重播当前队列  F=全屏切换  M=静音  Esc=退出

## UDP / OSC 控制（同一端口，默认 7000，绑定 0.0.0.0）

纯文本命令与 OSC 1.0 消息都收（按首字节区分），每条有回执：

| 文本命令 | OSC 地址 | 作用 |
|---|---|---|
| `play`/`pause`/`toggle` | `/play` `/pause` `/toggle` | 播放/暂停/切换 |
| `restart` | `/restart` | 重播当前队列 |
| `next`/`prev` | `/next` `/prev` | 切场 |
| `queue 2` | `/queue` (int) | 跳到指定队列（1 起） |
| `volume 0.5` | `/volume` (float 0~1) | 总音量 |
| `mute` | `/mute` | 静音切换 |
| `status` | `/status` | 返回队列/时间/fps/各层状态 |
| `quit` | `/quit` | 退出 |

OSC 支持 `#bundle`（执行第一个有效消息）、int32/float32 参数。

```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"queue 2", ("192.168.1.10", 7000))   # 文本命令，展项主机 IP
```

## 素材

**转 HAP（推荐格式）**：
```bash
deps/ffmpeg/bin/ffmpeg.exe -i input.mov -c:v hap -format hap_alpha out.mov
# Hap（无透明）: -format hap    Hap Q（更高画质无透明）: -format hap_q
# 转码速度：1080p ≈ 17× 实时，4K ≈ 4× 实时
```
支持：Hap / Hap Alpha / Hap Q / **Hap Q Alpha**（YCoCg+BC4 双纹理）、
Animation(QTRLE)、H.264（N 卡自动 NVDEC 硬解，失败回退软解）。不支持 BC7。
H.264/QTRLE 是 CPU 软解，只建议小分辨率点缀层；主力层务必 HAP。
注意 **ffmpeg 不能编码 Hap Q Alpha**（需 Vidvox 工具导出）；解码端有单元测试覆盖。

**音频（多源混音）**：每个带音频流的 HAP 层自动成为一个音源（跟随层的
入场/退出/循环，音量取图层行 `|音量%` 字段），外加 `[audio]` 外挂源（常开），
全部混音输出 48kHz stereo；视频主时钟 ±50ms 漂移校正。

## 性能实测（开发机：R7 6800H 8核16线程 + RTX3060 + 64GB + NVMe Gen4）

| 场景 | 冷机 | 热机（连续压测后） |
|---|---|---|
| 10 层 4K30 HAP Alpha（≈2.4GB/s 码率） | 80~124 fps | 6~13 fps |
| 15 层 4K30 HAP Alpha | 25~28 fps 起步 | 3~9 fps |
| 15 层 4K30 HAP（混合无 alpha 底层） | 同上量级 | 同左 |
| 10 层 4K HAP + 5 层 4K H.264 | 不可用（~1fps，RGBA 路径每层数据量是 HAP 的 4 倍） | — |
| 10 层 4K HAP + 5 层 1080p H.264（NVDEC） | 可跑，重负载层滞后不冻结 | 10~16 fps |
| 4 层 4K30 | 稳定 50+ fps | 稳定 |
| 10 层 720p | 100~160 fps | 无压力 |

- **alpha 不加成本**：Hap Alpha 与 Hap 每帧数据量相同（DXT5 8bpp），混合由 GPU 免费完成
- **热衰减巨大**：6800H 持续全核+GPU 满载约 10 秒后开始降频，冷热机数据差 2~3 倍。
  验收数字必须在目标主机上、跑满计划时长测
- 压测配置：`bench4k.txt`（2队列×10层4K）、`bench15.txt`、`bench15alpha.txt`（全 alpha）、
  `bench15mix.txt` / `bench15mix1080.txt`（HAP+H.264 混合）；素材在 `C:\hap4k_clips`

经验值：**稳定推荐 ≤4~6 层 4K 或 10+ 层 1080p**；H.264/QTRLE 只做 1080p 及以下点缀层。
10 层 4K 以上需要台式散热 + 独显直出（Optimus 混合输出有额外拷贝开销）。

## 排障开关（环境变量）

| 变量 | 作用 |
|---|---|
| `HAPPLAYER_NO_PBO=1` | 强制同步纹理上传（怀疑 PBO/fence 问题时用） |
| `HAPPLAYER_NO_NVDEC=1` | H.264 强制软解 |
| `HAPPLAYER_USE_VULKAN=1` | 启用实验性 Vulkan 渲染路径 |
| `HAPPLAYER_DEBUG_RING=1` | 状态行输出每层的 ring/pktQueue/gen/播放头（排障用） |

**日志化自检（免肉眼）**：
- `happlayer --verify-hapq <file.mov> <帧号> <ref.rgba>`：我们的 GPU 解码渲染一帧，
  与 ffmpeg CPU 解码的参考帧逐像素对比（验证 HAP Q 颜色公式与方向），PASS/FAIL 进日志。
  参考帧生成：`ffmpeg -i file.mov -vf "select=eq(n\,N)" -vframes 1 -f rawvideo -pix_fmt rgba ref.rgba`
- 启动日志回读 Win11 圆角策略（`回读策略=1` 即直角生效）

## 启动三件套

- `scripts/install-autostart.bat` / `uninstall-autostart.bat`：开机自启安装/移除
- 播放器内置：防休眠（ES_DISPLAY_REQUIRED）、健康监控（fps<5 持续 5s 或运行 7 天 →
  退出等外部拉起）、窗口位置记忆（%APPDATA%/happlayer/state.json，全屏退出不污染）

## 架构与关键设计（维护者必读）

```
解码线程(每层1个)                主线程(渲染)
  av_read_frame ─┐
                 ├→ pktQueue ─→ snappy worker ─→ ring(6帧) ─→ updateLayer 取帧 ─→ PBO ─→ GPU
H.264/QTRLE ─────┘   (HAP专用)   (解压+有序插入)                upload/drawLayer
音频: 每源解码线程 → 各源 PCM ring → SDL 回调混音 ← tick(主时钟逐源校正)
```

关键机制：
- **提交时定序**：帧序号与轮次 gen 在 packet 提交时绑定，worker 完成顺序不影响帧序；
  ring 按序号有序插入；reset/回卷时排空 pktQueue
- **presentIndex 闸门**：解码线程只允许领先播放头 RING_CAP 帧（否则 worker 全速绕文件，
  ring 全是"未来帧"，画面冻结/乱跳）
- **过期帧不解压**：worker 丢弃 `index < presentIndex-2` 的 job（主线程本来就只显示
  ≤播放头最新一帧；不跳过会陷入"渲染慢→解码全浪费→更慢"的死亡螺旋）
- **解码线程低优先级**（THREAD_PRIORITY_BELOW_NORMAL）：渲染线程永远先抢 CPU
- **循环回卷由主时钟驱动**：解码线程到 EOF 只挂起；主线程 fmod 回卷点发 pendingReset
- **预读 preRoll**：开播/切场/重播先缓冲 ≥2 帧再走队列时钟（超时 5s 兜底），
  预读结束复位健康监控计数避免误杀
- **热重载**：先解析新配置成功再销毁旧媒体层（改错配置不黑屏）

## 踩坑记录（反哺）

1. **Windows 计时器粒度 15.6ms**：`sleep_for(500us)` 实际睡 ~16ms，frame pacing 翻车
   （fps 锁 32）。必须 `timeBeginPeriod(1)`。
2. **Win11 无边框窗口也有圆角**：需 `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE=DWMWCP_DONOTROUND)`。
3. **混合显卡默认跑核显**：OpenGL 需导出 `NvOptimusEnablement` 才走独显；Optimus 独显渲染还要拷回核显上屏。
4. **MSVC 按 GBK 读 UTF-8 源文件**：中文注释直接编译错，CMake 加 `/utf-8`。
5. **MinGW 装在带空格路径下 ld 报错**；gyan FFmpeg 是 MSVC 库，直接用 MSVC 工具链。
   BtbN 包国内慢，gyan.dev 快；解 gyan 7z 用系统 `tar.exe`（py7zr 不支持 BCJ2）。
6. **Begin/End 内只有写 attribute location 0 才发射顶点**：多 attribute 必须
   `glBindAttribLocation` 显式绑定，否则部分 AMD/Intel 驱动花屏。
7. **HAP Q 的正确姿势**：Y 存 DXT5 的 alpha 通道，Co/Cg 存 R/G 要减 128/255 偏移，
   B 通道是 scale（`scale=B*(255/8)+1`），输出 alpha 必须置 1（否则 Y 被当透明度出鬼影）。
   Hap Q Alpha = 0x0D 多图帧（YCoCg DXT5 + BC4 alpha 两个子 section）。
8. **swr_convert 返回"每声道采样数"**：交错立体声要 ×声道数；音频 ring 全程按"采样点"计数。
9. **AVFormatContext 不是线程安全的**：切场 seek 用"重启信号"让解码线程自己做。
10. **HAP section 头解析**：`readSectionHeader` 内部已扣头部长度，多图帧遍历子 section
    时只再减数据长度（重复扣会越界）。
11. **笔记本压测必须考虑散热**：持续全核+GPU 满载约 10 秒后降频，数据差 2-3 倍；
    定型测试在目标主机上跑满计划时长。
12. **HAP 测试素材要加噪**：纯色合成片 snappy 压缩比极高，测不出真实负载。
13. **"过期帧不解压"必须留队尾活口**：解码慢于实时时，每个 job 到 worker 手上都已过期，
    全跳就永远冻屏。规则：只有队列里还有更新的 job 才跳过当前过期 job（快进到队尾照解）。
14. **切场/回卷要同步播放头闸门**（presentIndex 归 0），否则解码线程按上轮末尾的闸门
    一口气读到 EOF 挂起，新一轮无帧可播；预读就绪判断也必须校验 ring 里是当前轮的帧。
15. **glBegin/glEnd 内多 attribute 的顶点发射陷阱**：只有写 location 0 才发射顶点，
    先写 pos(loc 0) 再写 uv(loc 1) 会导致 UV 错位一个顶点——纯色测试看不出，
    真实内容画面旋转/镜像。必须先写 UV 再写 Pos。**教训：颜色/几何类 bug 要用
    有细节的素材 + 逐像素自动对比来验证**（见 --verify-hapq），纯色自测会漏。
16. **压测方法论**：测性能上限要用热机 + 长跑，报功能正确性用冷机短跑即可；
    对照组必须同机同温（同一配置隔几小时差 2~3 倍，全是散热变量）。

## 项目进度

**功能完成**（全部实测/单测验证）：
多队列编排、每层定时入场/循环/播完退出/自定义矩形/音量、场间淡入淡出、
多源音频混音（内嵌+外挂）、UDP + OSC 控制、多显示器/无边框/跨屏/缩放、
真热重载、HAP/HAP Alpha/HAP Q/HAP Q Alpha 解码、H.264（NVDEC）/QTRLE、
预加载、健康监控、防休眠、窗口位置记忆、开机自启脚本。

**验证状态**：
- 单元测试 6 项全绿（含 HAP 多 chunk、Hap Q Alpha 0x0D 多图帧）
- HAP Q 颜色/方向：`--verify-hapq` 逐像素对比 PASS（平均差 0.70，最大差 6）
- Win11 直角：启动日志回读 DWM 策略确认（`回读策略=1`）
- UDP 9 条文本命令 + OSC 7 条地址实测全通；双源混音、热重载、循环回卷实测通过

**待定**：
- 展项主机现场复测（10/15 层 4K 跑满时长、多屏拼接、接中控）
- Hap Q Alpha 端到端视觉验证（ffmpeg 无法编码该格式，等 Vidvox 工具导出的真实素材）
- Vulkan 路径：功能已补齐（淡入淡出用 1x1 黑场纹理等价实现、自定义矩形已对齐），
  Hap Q Alpha 的 alpha 平面不支持（告警+按不透明处理）；非默认路径，现场 GL 不达标再投入

## 目录结构

```
src/main.cpp       主程序（队列调度/解码线程/渲染/UDP+OSC/窗口/音频集成/shader/PBO/淡入淡出/verify 模式）
src/hapdecode.h    HAP 帧解码（单图/多 chunk/多图组合 0x0D，单测覆盖）
src/audio.{h,cpp}  音频子系统（SDL2 多源混音 + 主时钟逐源同步）
src/vulkan_*.{h,cpp}  Vulkan 实验渲染路径（HAPPLAYER_USE_VULKAN=1 启用）
tests/             单元测试（ctest）+ dump_hap 诊断工具
deps/              ffmpeg(gyan) / glfw / snappy / SDL2 / vulkan（不入库，见「第三方依赖」）
layers.txt         节目单（构建后自动拷到 exe 旁）
make_test_clips.bat  生成 720p 测试素材
bench*.txt         4K 压测配置（素材自行生成到 C:/hap4k_clips/）
scripts/           开机自启安装/移除、配置工具、SPIR-V 手工汇编生成器
docs/              用户指南 / 部署 / 排错 / 测试方案 / 变更日志
```

## 开源许可

本项目代码以 [MIT](LICENSE) 许可发布。

第三方组件各有自己的许可，随包分发二进制时请一并遵守：

- **FFmpeg**：默认使用 gyan.dev full 构建（GPL v3）；如需 LGPL 可换 `ffmpeg-release-lgpl-shared`。运行时动态链接，随播放器分发其 DLL 时需遵守对应许可并保留来源说明
- **snappy**（BSD）、**SDL2**（zlib）、**GLFW**（zlib）
- **HAP / Hap Q** 视频格式 by Vidvox
- HEVC/H.265 因专利池问题明确不支持、不引入
