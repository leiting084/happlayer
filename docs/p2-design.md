# P2 架构演进调研报告

> 项目：happlayer（HAP 多层视频播放器，C++17 + FFmpeg + GLFW + OpenGL）
> 日期：2026-08-13
> 状态：调研（不实施），用于决策 P2 阶段技术路线

---

## 0. 当前架构基线（P0+P1 已完成）

```
┌─────────────── 主线程（渲染+合成）───────────────┐
│  每队列 60 fps 调度                             │
│  ↓ 拉帧（lock）                                │
│  Layer.tex ← upload(buf)  ← PBO + fence 异步   │
│  glBegin QUADS 合成（YCoCg → HAP-Q shader）     │
└────────────────────────────────────────────────┘
         ↑ 帧数据                            ↓ swap
┌─────────────── 解码线程 × N ─────────────┐  ┌─────┐
│  Layer N decoder → HAP(DXT) / QTRLE / H264│  │ GPU │
│  ↓ ring buffer (RING_CAP=12 帧预读)      │  └─────┘
│  Hapdecode / FFmpeg 软解 + sws_scale    │
└────────────────────────────────────────┘
```

**已完成的 P0/P1 模块（不动）**：

| 编号 | 模块 | 收益 |
|------|------|------|
| P0.1 | PBO 异步纹理上传 | 15 层 4K 主线程 ~125ms → ~50-60ms |
| P1.1 | HAP Q YCoCg shader | DXT5 GPU 端转 RGB（无解码 CPU 开销） |
| P1.2 | 自适应帧率 | 负载高自动降 maxFps，保稳定 |
| P1.4 | HAP 自带音频 | MOV 自带音轨优先于外挂 wav |
| 多 codec | HAP + QTRLE + H.264 | 测试验证（见本文末附录） |

---

## P2.1 多 GPU 分担

### 背景

当前所有层的 `Layer.tex` 都在同一个 OpenGL 上下文中（默认 GPU）。N 台显示器若插在不同 GPU 上：
- Windows 默认情况：每个显示器的 3D 由接它的 GPU 负责渲染，跨 GPU 复制开销巨大
- 单 GPU 上 30+ 层 4K 时，纹理内存压力 + swap chain 调度争抢会成为瓶颈
- 多 GPU 分担让"渲染 GPU 1（顶层）+ 渲染 GPU 2（底层）"模式可显著降低单卡压力

### OpenGL 多上下文方案

#### 方案 A：多 HGLRC 共享纹理（WGL_NV_gpu_affinity / WGL_AMD_gpu_association）

| 维度 | 评估 |
|------|------|
| 原理 | 同一进程的多个 Render Context，共享 display list + texture + program |
| 优势 | 单进程管理；纹理创建一次可在所有 RC 用 |
| 劣势 | 需要 WGL 扩展（`WGL_NV_gpu_affinity` 或 `WGL_AMD_gpu_association`），驱动支持差异大；跨 GPU 纹理需 PBO + fence 显式同步 |
| 主线程数 | 2+（每 GPU 一个 render thread） |
| 同步 | `glFinish` / `glFenceSync` + `glClientWaitSync`；跨 GPU 额外需要 buffer copy（性能损耗） |
| 兼容 | Windows NVIDIA / AMD / Intel 三家扩展名不同，要 `#ifdef` 编译 |

#### 方案 B：分进程分 GPU（每个 GPU 一个 happlayer.exe 子进程）

| 维度 | 评估 |
|------|------|
| 原理 | 一个进程负责一层或一组层的上传 + 绘制，结果通过共享内存 / DXGI 输出到合成器 |
| 优势 | 进程隔离天然稳定；Windows 任务调度器自动选 GPU；崩溃不影响其他 GPU |
| 劣势 | 需要共享内存（spout / DXGI shared texture / NVIDIA NVLink 桥接）；进程间通信开销 |
| 主线程数 | N 个进程，每个进程自己 1 个主线程 |
| 同步 | 共享内存 spin-wait + 帧序号；VSync 在合成器侧 |
| 兼容 | Windows 自带 DXGI；NVIDIA 还有 NVAPI multi-GPU 优化 |

#### 方案 C：DXGI 1.2 Output Duplication + D3D11

| 维度 | 评估 |
|------|------|
| 原理 | 每 GPU 用 D3D11 VideoOutput 抓屏 → 共享纹理到合成 GPU → DXGI 1.2 Present 到对应输出 |
| 优势 | 系统级合成，OS 自动调度；崩溃优雅降级（GPU 掉了黑屏 + 报警） |
| 劣势 | 完全替换 OpenGL，需要重写 main loop；D3D11 比 GL 啰嗦 3 倍 |
| 主线程数 | 1 个合成线程 + N 个采集线程 |
| 同步 | D3D11 fence + event；Output Duplication 自身有 VSync 机制 |
| 兼容 | Windows 8+ 全支持 |

### 同步机制详细对比

```
WGL multi-RC 路径：
  GPU 0 context ──┐
                  ├─→ 共享 texture（CreateTexture 后 wglShareLists 同步）
  GPU 1 context ──┘
  跨 GPU 写：GPU 0 创建纹理 → GPU 0 glTexSubImage → GPU 1 可见（但跨 PCIE 拷贝，性能 -30%）
  跨 GPU fence 同步：必须 clientWaitSync + glFinish 后再访问
```

### 负载均衡策略

**静态分配**（推荐 P2.1 初期）：
- 启动时查询 `glfwGetMonitors()` → 关联 monitor → GPU vendor
- 队列 → GPU 映射表写在 layers.txt（每层显式指定 GPU 序号）
- 例：`0|1|gpu=1|clips/layer1.mov` 表示这层用 GPU 1

**动态分配**（P2.2 阶段）：
- 主进程 1Hz 采样每 GPU 的 `glGetInteger(GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX)` 
- 内存占用 >80% 自动把即将入场的层迁移到另一 GPU
- 迁移 = 把 HAP 解码线程产物传到目标 GPU 上下文 → 重新创建纹理 + 上传第一帧
- 迁移延迟 ~200ms（4K 单帧 8MB × 跨 PCIE）可接受

### 实施难点

| 难点 | 说明 | 解法 |
|------|------|------|
| WGL 扩展检测 | 不同 GPU vendor 扩展名不同 | `wglGetExtensionsStringARB` 列出 → 逐 vendor 编译分支 |
| 跨 GPU fence 同步 | 跨 PCIE 总线的 fence 需要明确等待 | `glClientWaitSync(fence, 0, GL_TIMEOUT_IGNORED)` |
| 共享纹理命名 | 多 RC 共享纹理需要名柄（ID 不够） | `glCreateMemoryObjectsEXT` + `glImportMemoryWin32HandleEXT`（需 GL 4.5） |
| 单例化 | 一个进程只能 init 一次 glfw | 多 RC 方案必须在 init 后再创建；分进程方案天然规避 |
| 共享音频 | 同一音频只有一个时钟源 | 音频仍由主进程播，副进程只负责视频纹理 |
| crash 隔离 | 单 RC crash 全崩 | 必须用分进程方案（A 方案 crash → 全黑屏） |

### 预计代码改动量

| 方案 | 新增文件 | 改动文件 | 工作量 |
|------|---------|---------|--------|
| A: WGL 多 RC | `src/glcontext.h/.cpp` | `main.cpp` (Layer 绑 GPU), `cmake` | 15-20 天 |
| B: 分进程 | `src/spout_sink.cpp`, `src/shmem_layer.h/.cpp` | `main.cpp`, 新建子进程 launcher | 25-30 天 |
| C: D3D11 Output Dup | 整个 `src/d3d11_*.cpp/h` | 重写主循环 50% | 60+ 天 |

### 推荐：方案 A（WGL 多 RC）+ 静态分配

- 投入产出比最高（15-20 天）
- 兼容现有 OpenGL 路径
- 单进程管理简单
- 缺点是跨 GPU 拷贝损耗 30%，但 4K 30fps 仍 60fps+ 跑得动（带宽 ~8GB/s × 30fps = 240GB/s，远低于 PCIE 4.0 ×16 = 32GB/s × 16 = 512GB/s）

---

## P2.2 Vulkan 渲染

### 背景

OpenGL 固定管线的限制：
- `glBegin/glEnd` + `glVertex2d` 这种 immediate mode 已经被 GL 3.0+ 标记为 deprecated
- 没有显式的 command buffer 概念，每帧渲染的开销大（驱动内部需要打包命令）
- 跨线程录制命令缓冲零成本（GL 完全单线程）
- 没有 compute shader（部分 sws_scale 可以用 GPU 加速）

Vulkan 的红利：
- 显式 command buffer 录制 = 零驱动开销
- 多线程并行 command buffer（每个 Layer 一个线程独立录）
- 计算着色器替代 sws_scale（H.264 → RGBA 转换可全 GPU）
- 现代 GPU 性能极限（HAP Q 解码端走 YCoCg-DXT5 → RGBA shader 替代 CPU sws）

### 替换 OpenGL 固定管线的关键步骤

#### 1. 上下文初始化

```cpp
// GLFW → Vulkan Surface
glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // 不要 GL 上下文
GLFWwindow* win = glfwCreateWindow(...);
VkSurfaceKHR surface;
glfwCreateWindowSurface(instance, win, nullptr, &surface);

// Physical device 选择
VkPhysicalDevice phys = pickPhysicalDevice(instance, surface);

// Queue family
VkQueueFamilyProperties props[2];
vkGetPhysicalDeviceQueueFamilyProperties(phys, &2_count, props);
graphics_q = findQueueFamily(VK_QUEUE_GRAPHICS_BIT);
present_q  = findQueueFamily(surface);
```

#### 2. Swapchain + RenderPass

```cpp
VkSwapchainCreateInfoKHR sci{};
vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
// image views per swapchain image
VkRenderPass rp = createRenderPass(format);  // 一个简单 clear + 1 subpass
VkFramebuffer fb[N] = createFramebuffers(swapchain_images, rp);
```

#### 3. Command Pool + Buffer per frame

```cpp
// 每帧一个 command buffer（双缓冲/三缓冲）
VkCommandPool pool[2];  // 实际在 P0.3 lock-free queue 后变成 per-thread pool
VkCommandBuffer cmd[2];
VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
ai.commandPool = pool[frame];
ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
vkAllocateCommandBuffers(device, &ai, &cmd[frame]);
```

#### 4. Pipeline（替代固定管线）

```cpp
// HAP/HAP-Q/RGBA 共用 pipeline layout（descriptor set：1 个 sampler2D）
// Vertex buffer = 全屏 quad（4 顶点）
// Index buffer = 0..3
// 多个 pipeline：
//   - HAP (DXT5)：标准纹理采样，无 shader 转换
//   - HAP Q (YCoCg)：fragment shader 内做 YCoCg → RGB
//   - RGBA：标准纹理采样
VkPipeline pipelines[3] = { ... };
```

#### 5. Draw（每层一次 draw call）

```cpp
VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
vkBeginCommandBuffer(cmd, &bi);

VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
rpbi.renderPass = rp;
rpbi.framebuffer = fb[image_index];
rpbi.renderArea = {{0,0}, {w,h}};
vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

for (Layer& L : layers) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[L.pipeline_idx]);
    vkCmdBindDescriptorSets(cmd, ..., L.descriptor_set);
    vkCmdBindVertexBuffers(cmd, 0, 1, &quad_vb, &offset);
    vkCmdBindIndexBuffer(cmd, quad_ib, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 4, 1, 0, 0, 0);
}

vkCmdEndRenderPass(cmd);
vkEndCommandBuffer(cmd);
vkQueueSubmit(graphics_q, 1, &submit, fence);
```

### HAP 压缩纹理在 Vulkan 中怎么上传

Vulkan 比 GL 强的地方：原生支持 DXT1/DXT5 压缩纹理（VkFormat `VK_FORMAT_BC1_RGB_UNORM_BLOCK` / `VK_FORMAT_BC5_SNORM_BLOCK` 等）— 不需要 S3TC 扩展。

```cpp
VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
ici.format = VK_FORMAT_BC3_UNORM_BLOCK;   // DXT5
ici.extent = {L.w, L.h, 1};
ici.mipLevels = 1;
ici.arrayLayers = 1;
ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
vkCreateImage(device, &ici, nullptr, &L.image);

// 设备内存（device local = GPU VRAM）
VkMemoryRequirements memreq;
vkGetImageMemoryRequirements(device, L.image, &memreq);
VkDeviceMemory mem;
vkAllocateMemory(device, allocInfo(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memreq), &mem);
vkBindImageMemory(device, L.image, mem, 0);

// staging buffer（host visible）→ 上传 DXT 数据
VkBuffer staging; ...
void* mapped = mapBuffer(staging);
memcpy(mapped, dxt_buf.data(), size);

// vkCmdCopyBufferToImage：异步上传
VkBufferImageCopy region{};
region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
region.imageExtent = {w, h, 1};
vkCmdCopyBufferToImage(cmd, staging, L.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

// layout transition：TRANSFER_DST → SHADER_READ_ONLY
// 录在另一个 command buffer 或用 barrier
```

### 着色器写法（SPIR-V）

HAP Q 的 fragment shader（与 GLSL 等价）：

```glsl
#version 450
layout(binding = 0) uniform sampler2D uTex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    vec4 yc = texture(uTex, vUV);   // .rgb = YCoCg, .a = 1
    float Y  = yc.r;
    float Co = yc.g;
    float Cg = yc.b;
    float r = Y - Co - Cg;
    float g = Y + Cg;
    float b = Y + Co - Cg;
    fragColor = vec4(r, g, b, yc.a);
}
```

`glslangValidator` 或 `glslc`（DXC）编译成 SPIR-V → `VkShaderModule`。

### SDL/Vulkan 与 OpenGL/GLFW 的兼容性

**关键决策**：GLFW 不能用 Vulkan + GL 混合（glfw 默认创建 GL 上下文）。

| 选项 | 评估 |
|------|------|
| **A. 弃 GL 用 Vulkan + SDL2** | ~~推荐。~~ **【2026-08-14 实践修正：未采用】** SDL2 原生支持 Vulkan surface（`SDL_Vulkan_CreateSurface`），且 SDL2 已有（音频依赖）。窗口/输入全 SDL2 管。 |
| **B. GLFW 创建无 API 窗口 + Vulkan surface** | ~~可行，但 GLFW 没有音频，要外挂 SDL2 音频，得两个窗口系统共存~~ **【2026-08-14 已验证】** `GLFW_NO_API` + Win32 surface 可用；音频继续走 SDL2 音频子系统（不是第二个窗口）。happlayer 阶段 E 即此路径。 |
| C. 保留 GLFW + Vulkan surface 扩展 | GLFW 3.3+ 有 `glfwCreateWindowSurface`，但音频还是得 SDL2 |

**推荐 A**：SDL2 窗口 + Vulkan 渲染 + SDL2 音频 — 三件统一。

**【2026-08-14 废弃·实践修正】** 上一句为设计期推荐。阶段 E 实测采用选项 B（GLFW_NO_API + Vulkan 渲染 + SDL2 仅音频），640×360 H.264/HAP 均锁 30 fps。选项 A 仍可作为未来整窗迁移候选，当前不需要。

```
### SDL2 + Vulkan 主循环：

```cpp
SDL_Window* win = SDL_CreateWindow(...);
SDL_Vulkan_CreateSurface(...);
while (!quit) {
    SDL_PollEvent(&e);
    vkWaitForFences(device, 1, &in_flight_fence, VK_TRUE, UINT64_MAX);
    vkAcquireNextImageKHR(...);
    vkResetCommandPool(...);
    recordCommandBuffer(cmd);
    vkQueueSubmit(...);
    vkQueuePresentKHR(present_q, &presentInfo);
}
```

### 工作量评估

| 模块 | 工作量 |
|------|--------|
| 上下文初始化（SDL2 + VkInstance + Device + Swapchain） | 5 天 |
| Pipeline 工厂（HAP / HAP-Q / RGBA 三套） | 3 天 |
| 纹理上传（DXT + RGBA + 多 PBO 替代） | 5 天 |
| Command buffer 录制（per-frame + per-layer） | 3 天 |
| Shader 编译 pipeline（SPIR-V + reflection） | 2 天 |
| Fence 同步 / present | 2 天 |
| 调试 + 错误处理（Vulkan 调试层） | 3 天 |
| 跨 GPU (P2.1 + Vulkan 组合) | 5 天 |
| 测试 + 性能回归 | 4 天 |
| **总计** | **~30 天** |

Vulkan 工作量比想象小，因为：
- 玩家用户场景固定（多层 alpha blend，无复杂几何）
- DXT 压缩纹理直接用 BC1/BC3 format，零扩展
- 单 surface + 单 queue family，简化程度 50%

---

## P2.3 NVDEC 硬解 H.264

### 背景

当前 H.264 软解在 4K 下：
- 测试显示 1920x1080 解码速度 30fps → 30+ fps 持续，但一帧 8MB RGBA 上传阻塞主线程
- 多层并发时 CPU 占用 80%+
- 单 GPU RTX 3060+ 都内置 NVDEC 专用电路（H.264/H.265/AV1）

NVDEC 红利：
- 解码速度 5-10 倍（4K H.264 30fps 从 30fps 提到 300fps+）
- 零 CPU 占用
- 直接出 NV12 到 GPU（DXGI/D3D11/Vulkan surface），省去 sws_scale

### NVIDIA Video Codec SDK 集成

#### 架构层级

```
┌──────────────────────────────────────────┐
│ FFmpeg 解封装 (avformat)                 │
│   ↓ 读 H.264 NALU packet                │
│ FFmpeg NVDEC 解码（cuvid hwaccel）       │
│   ↓ AVFrame (NV12 GPU memory)           │
│ sws_scale → RGBA（CPU 路径，省不掉）    │
│   或：Vulkan import → texture            │
└──────────────────────────────────────────┘
```

**两条集成路径**：

| 路径 | 说明 | 工作量 |
|------|------|--------|
| **A. FFmpeg NVDEC hwaccel（推荐）** | FFmpeg 自带 `h264_cuvid` 解码器，`avcodec_get_hw_config(codec, AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)` + `av_hwdevice_ctx_create(AV_HWDEVICE_CUDA)` | 3-5 天 |
| B. 直接 Video Codec SDK API | 用 `NvDecoder` 跳过 FFmpeg | 7-10 天（要自己解 NALU） |

#### 方案 A 详细代码

```cpp
// 初始化：创建 CUDA device context 给 FFmpeg 用
AVBufferRef* hw_device_ctx = nullptr;
av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);

// 设置 codecCtx 用 NVDEC
const AVCodec* codec = avcodec_find_decoder_by_name("h264_cuvid");  // NVIDIA NVDEC
// 或：avcodec_find_decoder(AV_CODEC_ID_H264) + get_format 回调（动态选 NVDEC）
AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
codecCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
avcodec_open2(codecCtx, codec, nullptr);

// 解码：API 调用方式不变
avcodec_send_packet(codecCtx, pkt);
avcodec_receive_frame(codecCtx, frame);
// frame->format 现在是 AV_PIX_FMT_CUDA（不是 YUV420！）
// frame->data[0] 是 CUDA 设备指针（不是 CPU 内存！）
```

#### sws_scale 从 NV12 CUDA → RGBA

**关键问题**：NVDEC 解出的 frame 是 CUDA device memory，sws_scale 默认读 CPU。需要：

```cpp
// 选项 1：先把 NV12 拷回 CPU 再 sws_scale（慢，违背 NVDEC 本意）
uint8_t* cpu_nv12 = (uint8_t*)av_malloc(w * h * 3 / 2);
cudaMemcpy(cpu_nv12, frame->data[0], w * h * 3 / 2, cudaMemcpyDeviceToHost);
sws_scale(sws, (const uint8_t* const[]){cpu_nv12}, ..., rgba);

// 选项 2：用 NPP（NVIDIA Performance Primitives）直接在 GPU 做 NV12 → RGBA
// 然后 cudaMemcpy 到 CPU 再上传（仍然有拷贝）
NppStatus np = nppYUVToRGB(...);

// 选项 3（推荐）：Vulkan + 外部内存 → 直接 import CUDA memory 到 Vulkan image
// 零拷贝！CPU 完全不参与
VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_NV;
VkImportMemoryWin32HandleInfoNV import_info{};
cudaMemcpyToArray(...); // 把 CUDA array → Vulkan image via WSI/NV专属扩展
```

**选项 3 是终极方案**，但需要 P2.2（Vulkan）先完成。

#### Vulkan + NVDEC 零拷贝路径

```cpp
// 1. FFmpeg NVDEC 解出 AVFrame (CUDA memory)
// 2. CUDA→Vulkan 互操作（VK_KHR_external_memory + VK_NV_external_memory_win32）
cudaExternalMemoryHandleDesc desc = {};
desc.handle.cudaMem = (void*)frame->data[0];  // CUDA pointer
desc.size = w * h * 3 / 2;
desc.type = cudaExternalMemoryHandleTypeOpaqueWin32;

// 3. Vulkan import 这个 memory 到 VkImage
VkImage vulkan_image;
vkCreateImage(device, ...);  // 共享
vkBindImageMemory(device, vulkan_image, imported_memory, 0);

// 4. Vulkan fragment shader 直接采样 NV12
// (NV12 shader 需要 GL_RED + GL_RED_INTEGER 或 VK_FORMAT_R8G8_UNORM 分两 plane 采样)
// 然后转 RGBA，整个过程零 CPU 参与
```

### 支持的 codec

| NVDEC 支持 | 状态 |
|----------|------|
| H.264 / AVC | ✅ 全功能（h264_cuvid） |
| H.265 / HEVC | ✅ 全功能（hevc_cuvid）— 但本项目策略禁用 HEVC（专利费） |
| AV1 | ✅ RTX 30+ 支持（av1_cuvid） |
| VP9 | ✅ RTX 20+ 支持（vp9_cuvid） |
| MPEG-2 | ✅ 老格式 |

**建议扩展**：H.264 + AV1 + VP9（继续禁 HEVC 与全局规则一致）

### 与 FFmpeg 软解的 API 兼容性

FFmpeg NVDEC 通过 `hw_device_ctx` 字段无缝切换：
- 同一份 avcodec_send_packet/receive_frame 代码
- 只是 `frame->format` 从 `AV_PIX_FMT_YUV420P` 变成 `AV_PIX_FMT_CUDA`
- 上层判断 `frame->format` 走不同路径：
  - `AV_PIX_FMT_CUDA` → Vulkan import 路径
  - 其他 → 现有 sws_scale 路径

零破坏性升级，旧的非 N 卡用户走 sws_scale 软解完全不受影响。

### 帧数据从 NVDEC → OpenGL 纹理的路径

**当前架构**：NVDEC CUDA memory → sws_scale（CPU） → RGBA8 → glTexSubImage2D（同步拷贝）

**问题**：跨 CPU 边界，违背 NVDEC 零拷贝初衷。P0.1 PBO 救不了 NVDEC 解码（CPU memcpy 那步就慢了）。

**P2.3 终极路径**（需 P2.2 Vulkan）：

```
H.264 packet
    ↓
FFmpeg h264_cuvid → AVFrame (CUDA device memory, NV12)
    ↓ cudaExternalMemory
Vulkan image (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, NV12 format)
    ↓ Vulkan fragment shader: NV12 → RGBA
OpenGL 看不到这一步（Vulkan 直接 Present）
    ↓ vkQueuePresentKHR
显示器
```

完全无 CPU 介入，NVDEC 解出 → GPU 上屏 = 2-4ms（4K）。

### 预计代码改动量

| 阶段 | 改动 | 工作量 |
|------|------|--------|
| FFmpeg NVDEC 集成 | `Layer` 加 hw_device_ctx 字段；openLayer 创建 CUDA device | 2 天 |
| NVDEC decode path | decoderRun 检测 codecCtx 走 NVDEC branch | 1 天 |
| CUDA → CPU NV12 → RGBA | sws_scale 处理 AV_PIX_FMT_CUDA frame | 2 天 |
| Vulkan + NVDEC 互操作（终极） | 需要 P2.2 完成 | +7 天 |
| 测试 + 性能回归 | benchmark 验证 4K 60fps 稳定 | 2 天 |
| **总计（不含 P2.2）** | | **7 天** |

---

## 优先级与依赖关系

```
P2.1 (多 GPU 分担)         15-20 天
P2.2 (Vulkan 渲染)          30 天
P2.3 (NVDEC 硬解)            7 天（CPU fallback）+ 7 天（Vulkan 零拷贝）= 14 天

依赖：
  P2.3 CPU 部分          ─ 独立可做
  P2.3 零拷贝部分        ─ 依赖 P2.2
  P2.1 + P2.2 组合       ─ 推荐 P2.1 静态分配 → P2.2 → P2.1 动态分配
```

**推荐实施顺序**：

1. **P2.3 (FFmpeg NVDEC 集成 + CPU fallback)** — 7 天，立即收益（CPU 占用降 60%）
2. **P2.1 (WGL 多 RC + 静态 GPU 分配)** — 15-20 天，30+ 层场景性能翻倍
3. **P2.2 (Vulkan 渲染)** — 30 天，最大性能潜力
4. **P2.3 (Vulkan 零拷贝)** — 7 天，NVDEC 终极利用

---

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| WGL 多 RC 驱动兼容差 | 保留 OpenGL 单 RC fallback |
| Vulkan 调试成本高 | 启用 VK_LAYER_KHRONOS_validation + RenderDoc |
| NVDEC 驱动崩溃 | FFmpeg 自动 fallback 到软解 |
| 重构期不能中断业务 | 双轨支持：环境变量 HAPPLAYER_USE_VULKAN=1 才走新路径 |

---

## 附录 A：当前多 codec 测试验证（2026-08-13）

### 测试环境

- 平台：Windows 11 + NVIDIA RTX（2560x1600 @240Hz 单显示器）
- 测试文件：
  - `clips/test_h264_small.mp4`：640x360 H.264 yuv420p 24fps
  - `clips/test_anim.mov`：1280x720 QTRLE argb 30fps

### 测试命令

```bash
deps/ffmpeg/bin/ffmpeg.exe -y -f lavfi -i "testsrc2=size=640x360:rate=24:duration=3" \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast ./clips/test_h264_small.mp4

deps/ffmpeg/bin/ffmpeg.exe -y -f lavfi -i "color=red:size=1280x720:rate=30:duration=4" \
  -pix_fmt rgba -c:v qtrle ./clips/test_anim.mov
```

### 运行结果（bench 6 秒）

```
[队列 1]
    test_h264_small.mp4  640x360  24fps  3s  start=0s  loop=是  [H.264]
    test_anim.mov       1280x720 30fps  4s  start=1s  loop=否  [QTRLE]

[bench] 6s 结束，平均 FPS ≈ 32
  队列1 test_h264_small.mp4: 解码 313 帧    ← H.264 路径
  队列1 test_anim.mov:      解码 132 帧    ← QTRLE 路径
```

**结论**：
- ✅ H.264 FFmpeg 软解路径正常（解码 313 帧，循环显示）
- ✅ QTRLE 路径正常（解码 132 帧）
- ⚠️ 同步 RGBA 上传限制主线程 fps ~32（HAP PBO 路径 60fps+）— 待 P2.3 NVDEC 或 P0.2 RGBA-PBO 优化

### 已知限制

1. RGBA 同步上传慢（4K = 8MB/帧 × 30fps = 240MB/s 阻塞主线程）
   - 解法：P0.2 加 RGBA PBO 上传（类似现有 DXT 路径），5 天工作量
   - 或：P2.3 NVDEC + Vulkan 零拷贝，14 天工作量
2. 多 codec 共享同一解码线程池（H.264 多层时 CPU 100%）
   - 解法：P2.3 NVDEC 接管，或 decoder thread_count=auto 利用多核

---

## 附录 B：参考资料

| 主题 | 链接 |
|------|------|
| NVIDIA Video Codec SDK | https://developer.nvidia.com/video-codec-sdk |
| FFmpeg HWAccel Intro | https://trac.ffmpeg.org/wiki/HWAccelIntro |
| Vulkan Tutorial | https://vulkan-tutorial.com/ |
| WGL multi-GPU | https://www.khronos.org/registry/OpenGL/extensions/NV/WGL_NV_gpu_affinity.txt |
| Khronos VK_KHR_external_memory | https://www.khronos.org/registry/vulkan/specs/1.3-extensions/man/html/VK_KHR_external_memory.html |
| CUDA-Vulkan Interop | https://github.com/nvpro-samples/gl_vk_raytraced_interop |

---

## 纠错记录

### 2026-08-14：推荐选项 A 未采用，阶段 E 验证选项 B

- **原文问题**：P2.2 写「推荐 A：弃 GL 用 Vulkan + SDL2」，并暗示 GLFW+Vulkan 要「两个窗口系统共存」。
- **纠正原因**：happlayer 阶段 E 在现有 GLFW 窗口上设 `GLFW_NO_API`，Vulkan 渲染 + 原 SDL2 音频子系统即可。同一 HWND 上 GL+VK 会 `0xC0000005`；窗口必须一开始就是 NO_API。
- **修正后内容**：当前实现 = 选项 B。选项 A 保留为未来整窗迁移候选。详见 `docs/changelog/2026-08-14-vulkan-stage-e.md`。
- **额外教训**：`vkCreateShaderModule` 成功 ≠ `vkCreateGraphicsPipelines` 能建；旧嵌入 SPIR-V 返回 `-13`。