// vulkan_renderer.h — Vulkan 阶段 D+E：多 layer 完整渲染管线
//
// 模块定位：
//   - **可切换路径**：仅当 HAPPLAYER_HAS_VULKAN 宏启用时参与编译
//   - **运行时可选**：仅当环境变量 HAPPLAYER_USE_VULKAN 被设置时调用
//   - **不替换 OpenGL**：默认 OpenGL 路径不变，Vulkan 是可选加速/对照路径
//
// 阶段 D+E 范围：
//   - 多层 HAP / H.264 / Animation 视频叠加（每层独立 descriptor set）
//   - 双 pipeline：HAP Q（YCoCg→RGB）+ RGBA 直通
//   - 逐帧 DXT/RGBA 上传：draw 前 recordCopyToImage（持久 staging）
//   - 每层 viewport/scissor 按 rect 定位（替代 vert shader 读 UBO）
//   - 预分配 descriptor set（避免每帧耗尽 pool）
//
// 范围外（YAGNI）：
//   - 多线程 command buffer 录制
//   - swapchain 重建（窗口 resize 会失败，符合"简化版"约定）
//   - 验证层 / debug messenger
//
// 安全约定：
//   - init 失败返回 false（不抛异常），Renderer 保持未初始化状态
//   - drawFrame 在未初始化时返 false
//   - shutdown 幂等（多次调用安全），所有 VK_NULL_HANDLE 安全
//   - 同进程仅一个 Renderer 实例（单例）
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace vk_hapq { struct DXTImage; }

namespace vk_render {

// Vulkan 渲染器（多 layer 完整版）
class Renderer {
public:
    // 初始化：创建 instance / device / swapchain / pipeline / descriptor pool
    // hwnd: Win32 窗口句柄（用 glfwGetWin32Window 获取）
    // w, h: 窗口尺寸（用于 swapchain）
    // 返回是否成功
    bool init(void* hwnd, int w, int h);

    // 绘制一帧：可选 transfer → 清屏 → 遍历 layers → bind pipeline → drawIndexed(6)
    // layers: 多层视频描述（image view + 是否 HAP Q + 屏幕 rect）
    // 返回是否成功（swapchain out-of-date / 窗口 resize 等场景会失败）
    // 当 imageView == VK_NULL_HANDLE 时，本帧该层被跳过（不绘制）。
    struct DrawLayer {
        VkImageView imageView   = VK_NULL_HANDLE;   // 该层纹理（DXT 或 RGBA）
        vk_hapq::DXTImage* tex = nullptr;           // dirty 时用于 GPU copy
        bool        dirty       = false;            // 本帧 staging 有新数据
        bool        useHapQ     = false;            // true=HAP Q pipeline, false=RGBA pipeline
        // 屏幕坐标（像素，窗口坐标系：左上原点，Y 向下）
        float       rectX       = 0.0f;
        float       rectY       = 0.0f;
        float       rectW       = 0.0f;
        float       rectH       = 0.0f;
        // UV 裁剪（0-1，源纹理坐标系）
        float       uvX0        = 0.0f;
        float       uvY0        = 0.0f;
        float       uvX1        = 1.0f;
        float       uvY1        = 1.0f;
    };
    bool drawFrame(const std::vector<DrawLayer>& layers);

    // 等待 GPU 完成（vkDeviceWaitIdle）
    void waitIdle();

    // 清理（幂等）
    void shutdown();

    // 是否已初始化（外部 GL 主循环可借此判断是否走 Vulkan 路径）
    bool isInitialized() const { return initialized_; }

    // 阶段 E：暴露给 main.cpp createTexture() 用
    VkDevice         getDevice()       const { return device_; }
    VkPhysicalDevice getPhysDevice()   const { return physDevice_; }
    VkCommandPool    getCommandPool()  const { return cmdPool_; }
    VkQueue          getGraphicsQueue()const { return graphicsQueue_; }

    // 单例（happlayer 全局共用一个 Renderer）
    static Renderer& instance();

private:
    Renderer() = default;
    ~Renderer() { shutdown(); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 最大层数（descriptor pool 容量 = kMaxLayers）
    static constexpr uint32_t kMaxLayers = 32;

    bool initialized_ = false;

    // Core
    VkInstance       instance_       = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_     = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_        = VK_NULL_HANDLE;
    VkQueue          graphicsQueue_  = VK_NULL_HANDLE;
    VkQueue          presentQueue_   = VK_NULL_HANDLE;
    uint32_t         graphicsFamily_ = UINT32_MAX;
    uint32_t         presentFamily_  = UINT32_MAX;

    // Swapchain
    VkSwapchainKHR           swapchain_    = VK_NULL_HANDLE;
    VkFormat                 swapchainFmt_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapchainExt_{};
    std::vector<VkImage>     swapImages_;
    std::vector<VkImageView> swapViews_;

    // Render pass + framebuffers
    VkRenderPass                renderPass_     = VK_NULL_HANDLE;
    std::vector<VkFramebuffer>  framebuffers_;

    // 全屏 quad（共享 vertex + index buffer）
    VkBuffer       quadVB_       = VK_NULL_HANDLE;
    VkDeviceMemory quadVBMem_    = VK_NULL_HANDLE;
    VkBuffer       quadIB_       = VK_NULL_HANDLE;
    VkDeviceMemory quadIBMem_    = VK_NULL_HANDLE;

    // HAP Q pipeline（YCoCg DXT5 → RGB）
    VkPipeline              hapQPipeline_       = VK_NULL_HANDLE;
    VkPipelineLayout        hapQLayout_         = VK_NULL_HANDLE;
    VkDescriptorSetLayout   hapQDescLayout_     = VK_NULL_HANDLE;

    // RGBA pipeline（普通 sampler2D → RGBA）
    VkPipeline              rgbaPipeline_       = VK_NULL_HANDLE;
    VkPipelineLayout        rgbaLayout_         = VK_NULL_HANDLE;
    VkDescriptorSetLayout   rgbaDescLayout_     = VK_NULL_HANDLE;

    // 默认 sampler（所有 layer 共享，linear filter + clamp to edge）
    VkSampler                defaultSampler_     = VK_NULL_HANDLE;

    // Descriptor pool + 预分配 set（kMaxLayers × kMaxFramesInFlight）
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> layerDescSets_;

    // Per-layer UBO 池（kMaxLayers 个独立的 VkBuffer）
    struct LayerUBO {
        VkBuffer       buffer     = VK_NULL_HANDLE;
        VkDeviceMemory memory     = VK_NULL_HANDLE;
        void*          mapped     = nullptr;  // 持久映射（每帧 memcpy）
    };
    std::vector<LayerUBO> layerUBOs_;

    // Command
    VkCommandPool   cmdPool_     = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer_   = VK_NULL_HANDLE;

    // Sync
    static constexpr uint32_t kMaxFramesInFlight = 2;
    std::vector<VkSemaphore>   imageAvailable_;   // size = kMaxFramesInFlight
    std::vector<VkSemaphore>   renderFinished_;  // size = kMaxFramesInFlight
    std::vector<VkFence>       inFlightFences_;  // size = kMaxFramesInFlight
    std::vector<VkFence>       imagesInFlight_;  // size = swapImages_.size()
    uint32_t currentFrame_ = 0;
};

} // namespace vk_render
