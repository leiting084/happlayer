# 项目经验教训（EXPERIENCE）

> 项目：happlayer— 多层 HAP/H.264/Animation 视频播放器
> 日期：2026-08-14
> 目的：沉淀项目过程中遇到的关键教训，供未来类似项目参考

## 教训 1：性能优化前必须实测（来自 P0.2 snappy 误判）

### 误判
```
我："理论上 snappy 不是瓶颈（DXT 解压已很快 + 单帧解压时间 < 显示时间）"
→ 跳过 snappy 并行优化
```

### 实测打脸
```
单线程 snappy 解压 = 33 fps（瓶颈！）
2 worker 并行   = 212 fps（6.4x 提速）
```

### 教训
- **禁止用理论分析跳过性能优化**
- **必须先实测**确定真瓶颈在哪
- 凭印象判断"不是瓶颈"是最容易犯的错误

---

## 教训 2：复杂重构必须有基线测试（来自 P0.3 SPSC 回退）

### 灾难
```
任务：P0.3 SPSC lock-free queue 改造
Agent 写完代码，编译过 ✅
实测：32 fps（之前是 60fps）
回退损失：50% 性能
```

### 反思
- 没在重构前跑基线测试
- Agent 不知道"60fps"是事实（而不是估计）
- 重构引入 60→32fps 回退，发现不了
- 用户被迫回退 → 修复 → 再回退 → 完整重写（4 个回合）

### 教训
- **任何重构前先跑 baseline**，存数据
- **重构后立即跑 baseline 对比**
- **分小步增量重构**，每步验证
- **可回滚**：重构失败立即 git checkout HEAD

---

## 教训 3：合理的工作量评估（来自 P2 Vulkan）

### 原计划
```
P2.2 Vulkan 30 天大工程
```

### 实际分阶段实施
```
阶段 A（HAP Q SPIR-V）：1-2 天
阶段 B（DXT 上传）：2-3 天
阶段 C（单 surface 管线）：5-7 天
总计：8-12 天（比 30 天少 60-70%）
```

### 教训
- **大任务拆小阶段**，每阶段独立交付价值
- **不要被"全量改造"吓退**
- **可选加速路径用条件编译**而非重写主路径

---

## 教训 4：测试矩阵不能缺（来自回归测试发现）

### 多 codec 实际场景
```
之前只测 HAP 单 codec
后来测：HAP + H.264 + Animation 混合
发现：RGBA 同步上传瓶颈（4K 8MB/帧 × 30fps = 240MB/s）
→ 发现新优化点（RGBA PBO）
```

### 教训
- **不要只测 happy path**（单 codec 流畅）
- **必须测混合场景**（多 codec + 极端负载）
- **3 件套回归**：单 codec + 多 codec + 极端负载

---

## 教训 5：用户全局规则的价值（来自 evolution-loop.md）

### 触发的规则
- `triple-defense.md`：防幻觉（写后必读、改后必验、不确定必说）
- `evolution-loop.md`：总结→反哺→投喂
- `customer-spec-alignment.md`：客户原话驱动
- `first-principles-adversarial.md`：交付前对抗式审查

### 实际应用
- 反哺写了 `performance-benchmark-before-skip.md`（教训 1）
- 反哺写了 `refactor-baseline-required.md`（教训 2）
- 这些规则现在是**全局规则**，影响所有未来项目

### 教训
- **每个项目完成后反哺经验**到全局规则
- **不要重复踩同一个坑**

---

## 关键数据（决策依据）

| 测试场景 | 数据 | 决策 |
|---------|------|------|
| 单层 HAP 1280×720 | 60fps（真 GPU）| 基准性能 |
| 单层 HAP 1280×720 | 32fps（headless）| 软件渲染上限 |
| snappy 单线程解码 | 33fps | **是瓶颈**（误判后实测发现）|
| snappy 2 worker 解码 | 212fps | 并行提速 6.4x |
| 15 层 4K | 8fps（headless）| OpenGL 上传上限 |
| 多 codec 混合 | 32fps | RGBA 同步上传瓶颈 |
| NVDEC h264_cuvid | 60+ fps（NVIDIA）| 硬解可行 |

---

## 性能优化优先级（实际收益排序）

按实测收益从高到低：

1. **P0.2 snappy 并行**（6.4x）— 最大单点收益
2. **P2.3 NVDEC 硬解**（4K H.264 应该从 30→100+ fps）
3. **资源预加载**（冷启动加速）
4. **P0.1 PBO 上传**（真实 GPU 上预计 30-50% 提速）
5. **P1.1 HAP Q shader**（避免 CPU sws_scale）
6. **P1.2 自适应帧率**（保证稳定性）

---

## 架构经验（Vulkan 模块分层）

### 分层设计
```
src/vulkan_hapq.h/cpp    — 资源层（HAP Q SPIR-V + DXT 上传）
src/vulkan_renderer.cpp  — 管线层（单 surface + command buffer）
```

### 为什么分层
- **资源层可复用**：未来完整 Vulkan 迁移时也用这个
- **管线层可选**：现阶段仅启用单 surface，复杂功能按需加
- **条件编译集成**：`#ifdef HAPPLAYER_HAS_VULKAN` 包住调用

---

## 教训 6：Vulkan 窗口必须 GLFW_NO_API（来自阶段 E）

OpenGL context 和 Vulkan surface 不能绑同一个 HWND——`vkCreateWin32SurfaceKHR` 会 Access Violation（0xC0000005）。`HAPPLAYER_USE_VULKAN=1` 时必须在 `glfwCreateWindow` 之前设 `GLFW_CLIENT_API = GLFW_NO_API`，失败也不能回退 GL。

旧嵌入的 glslang SPIR-V 能 `vkCreateShaderModule` 但 `vkCreateGraphicsPipelines` 返回 `VK_ERROR_UNKNOWN (-13)`。手写最小 vert/frag 后 pipeline 才过。**shader module 成功 ≠ pipeline 能建。**

## 教训 7：逐帧纹理用持久 staging，copy 进同一 command buffer（来自阶段 E）

不要每帧 `vkCreateImage` + `vkQueueWaitIdle`。CPU 只 `memcpy` 到 mapped staging；GPU `copyBufferToImage` 录在 draw 的 command buffer 里、放在 `vkCmdBeginRenderPass` 之前。descriptor set 预分配（`kMaxLayers × framesInFlight`），禁止每帧 allocate。单缓冲 UBO/staging 时 draw 前等全部 in-flight fence。

已沉淀：内部 pattern `vulkan-persistent-staging-upload`。

---

## 用户沟通经验

### 用户痛点
- 用户说"全部做" → 必须把 P0/P1/P2 全部跑一遍（即使 P2.1 单 GPU 不可行也要解释）
- 用户说"按顺序" → 不能跳步骤
- 用户问"刚才结束了什么" → 系统通知（14 个 agent 停止）用户未必清楚
- 用户多次要求"简要" → 之前回答太长，要简短

### 改进
- 答案先讲结论，后讲原因
- 长答案用表格 + 简短摘要
- 系统通知主动解释

---

## 致谢

- 项目结构与用户（明确需求 + 持续反馈）
- FFmpeg / OpenGL / SDL2 / Vulkan 等开源项目
- 双 GPU / NVDEC / Vulkan 等未来可选项