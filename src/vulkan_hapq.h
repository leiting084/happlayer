// vulkan_hapq.h — Vulkan 阶段 A+B+E：HAP Q shader + DXT/RGBA 纹理上传
//
// 本模块是**可切换路径**（不替换 OpenGL 渲染管线）。
// 阶段 A：HAP Q shader SPIR-V 字节码（YCoCg DXT5 → RGB）
// 阶段 B：DXT 压缩纹理创建（staging + optimal image）
// 阶段 E：持久 staging + 逐帧 memcpy；GPU copy 由 renderer 在 draw 前 recordCopyToImage
//
// 设计目标：
//   - 仅编译：默认 HAPPLAYER_HAS_VULKAN 宏启用后参与编译，不主动调用
//   - Vulkan 1.0+ 兼容：使用最基础的 API，不依赖 Vulkan 1.1/1.2 特性
//   - 不引入任何额外依赖（除 vulkan-1.lib）
//
// 安全约定：
//   - 创建函数失败返回 VK_NULL_HANDLE / 0（不抛异常）
//   - 销毁函数接受 VK_NULL_HANDLE 安全（idempotent）
//   - createTexture 失败时返回的 DXTImage 各字段都是 VK_NULL_HANDLE / 0
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace vk_hapq {

// -----------------------------------------------------------------------------
// 阶段 A：shader SPIR-V 字节码
// -----------------------------------------------------------------------------
//
// GLSL 源码（仅参考，实际以嵌入的 SPIR-V 字节码为准）：
//
//   // 顶点 shader（屏幕坐标 + UV）
//   #version 450
//   layout(location=0) in vec2 aPos;
//   layout(location=1) in vec2 aUV;
//   layout(location=0) out vec2 vUV;
//   void main() {
//       gl_Position = vec4(aPos, 0.0, 1.0);
//       vUV = aUV;
//   }
//
//   // 片段 shader（HAP Q YCoCg → RGB）
//   #version 450
//   layout(binding=0) uniform sampler2D uTex;
//   layout(location=0) in vec2 vUV;
//   layout(location=0) out vec4 fragColor;
//   void main() {
//       vec4 yc = texture(uTex, vUV);
//       float Y = yc.r, Co = yc.g, Cg = yc.b;
//       fragColor = vec4(Y - Co - Cg, Y + Cg, Y + Co - Cg, yc.a);
//   }
//
//   // 片段 shader（RGBA / DXT1 / Hap Alpha 直通）
//   #version 450
//   layout(binding=0) uniform sampler2D uTex;
//   layout(location=0) in vec2 vUV;
//   layout(location=0) out vec4 fragColor;
//   void main() { fragColor = texture(uTex, vUV); }
//
extern const std::vector<uint32_t> kHapQVertSpirv;
extern const std::vector<uint32_t> kHapQFragSpirv;
extern const std::vector<uint32_t> kRgbaFragSpirv;
extern const std::vector<uint32_t> kSimpleVertSpirv;
extern const std::vector<uint32_t> kHapQYcocgFragSpirv;

// 创建 HAP Q shader pipeline
//   device      : 已初始化的 VkDevice
//   renderPass  : 调用方提供的 render pass（兼容的颜色附件 + subpass 0）
//   outLayout   : 输出 pipeline layout（调用方需保存以备销毁）
//   outDescLayout: 输出 descriptor set layout（仅绑定 0 = 1 个 sampler）
// 返回：VkPipeline，VK_NULL_HANDLE = 失败
VkPipeline createHapQPipeline(VkDevice device,
                              VkRenderPass renderPass,
                              VkPipelineLayout* outLayout,
                              VkDescriptorSetLayout* outDescLayout);

// 销毁（幂等：VK_NULL_HANDLE 安全）
void destroyHapQPipeline(VkDevice device,
                         VkPipeline pipeline,
                         VkPipelineLayout layout,
                         VkDescriptorSetLayout descLayout);

// -----------------------------------------------------------------------------
// 阶段 B+E：纹理（DXT 压缩 / RGBA 未压缩）
// -----------------------------------------------------------------------------
//
// createTexture：创建 optimal image + 持久映射 staging，把第一帧 memcpy 进去。
//   **不提交 GPU copy**——由 renderer::drawFrame 在 render pass 之前
//   调用 recordCopyToImage，避免每层每帧 vkQueueWaitIdle。
//
// updateTexture：后续帧只 memcpy 到已有 staging（CPU 侧）。失败返回 false。
struct DXTImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    uint32_t       width  = 0;
    uint32_t       height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    size_t         dataSize = 0;

    // 阶段 E：持久 staging（host visible，mapped 直到 destroy）
    VkBuffer       stagingBuf    = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem    = VK_NULL_HANDLE;
    void*          stagingMapped = nullptr;
    size_t         stagingCap    = 0;
    bool           layoutUndefined = true;  // true = 尚未做过 GPU copy
};

// 支持的 format：
//   VK_FORMAT_BC1_RGB_UNORM_BLOCK  (HAP DXT1)
//   VK_FORMAT_BC3_UNORM_BLOCK      (Hap Alpha / HAP Q)
//   VK_FORMAT_R8G8B8A8_UNORM       (H.264 / QTRLE)
DXTImage createTexture(VkDevice device,
                       VkPhysicalDevice physDevice,
                       const void* data,
                       size_t dataSize,
                       uint32_t width,
                       uint32_t height,
                       VkFormat format);

// 把新一帧 memcpy 进已有 staging。尺寸必须与创建时一致。
bool updateTexture(DXTImage& img, const void* data, size_t dataSize);

// 在已 begin 的 command buffer 里录制：barrier → copyBufferToImage → barrier
// 调用方负责在 render pass 之外调用；完成后把 img.layoutUndefined = false。
void recordCopyToImage(VkCommandBuffer cmd, const DXTImage& img);

// 兼容阶段 B 旧名：等同 createTexture（不再内部 submit/wait idle）
DXTImage uploadDXT(VkDevice device,
                   VkPhysicalDevice physDevice,
                   VkCommandPool cmdPool,
                   VkQueue queue,
                   const void* data,
                   size_t dataSize,
                   uint32_t width,
                   uint32_t height,
                   VkFormat format);

// 销毁（幂等：所有字段 VK_NULL_HANDLE/0 安全）
void destroyDXT(VkDevice device, DXTImage& img);

} // namespace vk_hapq
