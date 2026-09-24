# 2026-09-16 发布前重建 + 交付核查 + 现场验收清单

> 触发：用户问"这个项目确实开发完了？"，做交付前对抗式核查。
> 类型：发布候选（RC）核查 / 文档补债 / 验收准备。

## 🔴 核心发现：交付的 exe 曾是过期二进制（3 天断档）

| 项 | 时间戳 |
|---|---|
| `src/main.cpp` + 4 个 `vulkan_*.{cpp,h}` 最后修改 | **2026-08-20 20:04** |
| `build/Release/happlayer.exe`（核查前） | **2026-08-17 09:17** |
| README.md 最后修改 | 2026-08-20 20:04（与源码同批） |

即：**最后一轮源码改动（8-20）从未被编译**，Release 目录里的旧 exe 不含该轮改动。
若当时直接拷走部署，跑的是旧版本；README 描述的最终功能也从未在该二进制上验证过。

### 处置（今天已执行）

1. ✅ `cmake --build build --config Release` 全量增量重建 → 0 error 0 警告
   - 新 `happlayer.exe` = **2026-09-16 04:44**，460800 字节
2. ✅ `ctest -C Release` → 100%（手写 6 用例：无压缩/snappy DXT5/DXT1/多 chunk/拒 BC7/Hap Q Alpha 0x0D）
3. ✅ 直接跑 `test_hapdecode.exe` → 6/6 PASS
4. ✅ 新 exe bench 实测：`layers.txt --bench 2 --port 0 --size 960x540` 平均 59 fps，
   2 队列调度、Win11 直角回读策略=1、JSON 报告均正常
5. ✅ 功能字符串/代码核查：OSC（/queue /volume 等，main.cpp:1932-1991）、fade、
   --verify-hapq、热重载、多源混音（audio.h：48kHz 多源）、preload/max_duration、
   HAPPLAYER_DEBUG_RING 均在最终源码中

## 文档债处理

- `docs/CHANGELOG.md`：追加 **v1.3** 节，补录 8-16~8-20 落地但漏记的功能
  （多源混音、OSC、Hap Q Alpha、fade、自定义矩形、verify-hapq、preload、Vulkan 补齐）。
  补录依据 = README(8-20) + 今天对源码的 grep/实测，非凭印象。
- `docs/USER_GUIDE.md`：文末追加「纠错记录（2026-09-16）」，3 处错误显式对照
  （遵循项目"纠错用增补不用覆盖"规则，保留原文）：
  1. `-format alpha_hap / q_hap` 错，实测 ffmpeg 只认 `hap_alpha / hap_q`
  2. "[audio] 段在内嵌音频时被忽略" 过时，最终版是**多源混音**
  3. 性能表"15 层 4K 60fps / 30 层 50-55fps"与 README 热机实测（15 层 4K 热机 3~9fps）矛盾
- 新建 `docs/ON_SITE_ACCEPTANCE_CHECKLIST.md`：现场验收勾选项。

## 仍未闭环（不算 GA，只是 RC）

- 展项主机现场复测：10/15 层 4K 跑满计划时长、多屏拼接、接中控 UDP/OSC
- Hap Q Alpha 真实素材（Vidvox 导出）端到端视觉验证，ffmpeg 无法编码该格式
- Vulkan 仍是实验路径：无 resize、HQA alpha 平面不支持（按不透明告警）、init 失败直接退出
- 4K 压测素材在开发机 `C:\hap4k_clips`，换机 bench 配置缺素材
- 纯净展项主机可能需装 VC++ 2015-2022 x64 运行时

## 反哺点

1. **交付前必须做"时间戳对账 + 干净重建"**：源码 mtime > 产物 mtime 就是断档信号。
   "我记得编译过 / bench 跑通过"不算数——可能跑的是旧二进制（本次实际就跑了一次旧 exe）。
   待沉淀为通用检查项（建议下次确认后沉淀为通用 pattern 或交付 SOP）。
2. **文档与代码会一起过期**：USER_GUIDE 三处错误都是"中间版本认知"残留（单音源、
   旧 ffmpeg 参数名、乐观性能数字）。交付文档以最终代码 + 实测为准，旧文档只作历史。
3. **"开发完成" ≠ "验收完成"**：展项软件的验收发生在目标硬件上（热衰减、多屏、中控），
   开发机绿测只是 RC 门槛。
