// vulkan_renderer.cpp — Vulkan 阶段 D+E：多 layer 完整渲染管线实现
//
// 阶段 D+E 范围：
//   - 单 surface / 单 swapchain / 单 render pass
//   - 两条 graphics pipeline：HAP Q（YCoCg）+ RGBA 直通
//   - 逐帧纹理上传：render pass 前 recordCopyToImage
//   - 每层 viewport/scissor 按 rect 定位
//   - 预分配 descriptor set（kMaxLayers × frames in flight）
//   - 单线程 command buffer 录制
//
// 与 vulkan_hapq.cpp 的关系：
//   - **不复用** vk_hapq::createHapQPipeline（它的 descLayout 只有 sampler2D，没有 UBO）
//   - 直接 include vulkan_hapq.h 拿 SPIR-V 字节码
//   - 纹理：createTexture / updateTexture / recordCopyToImage

// 必须在 vulkan.h 之前：启用 Win32 surface 平台类型
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#  include <windows.h>
#endif

#include "vulkan_renderer.h"
#include "vulkan_hapq.h"  // 取 kHapQVertSpirv / kHapQFragSpirv 字节码

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// 解决 Windows <windows.h> 把 max/min 定义为宏，与 std::max/min 冲突
#ifdef max
#  undef max
#endif
#ifdef min
#  undef min
#endif

namespace vk_render {

// -----------------------------------------------------------------------------
// 内部辅助
// -----------------------------------------------------------------------------

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    const char* lvl = (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
                    : (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                    : "INFO";
    std::fprintf(stderr, "[vulkan %s] %s\n", lvl, data && data->pMessage ? data->pMessage : "?");
    return VK_FALSE;
}

static bool checkVk(VkResult r, const char* op) {
    if (r == VK_SUCCESS) return true;
    std::fprintf(stderr, "[vulkan_renderer] %s 失败: VkResult=%d\n", op, (int)r);
    return false;
}

static bool hasStencilComponent(VkFormat fmt) {
    return fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
           fmt == VK_FORMAT_D16_UNORM_S8_UINT;
}

// 找支持给定 memory type 的 memory type index
static uint32_t findMemoryType(VkPhysicalDevice physDevice,
                               uint32_t bits,
                               VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

// 选物理设备：优先 discrete GPU，否则任意
static VkPhysicalDevice pickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance, &count, devs.data());

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return d;  // 首选
        }
        if (fallback == VK_NULL_HANDLE) fallback = d;
    }
    return fallback;
}

// 找支持 graphics + present 的队列族
static std::pair<uint32_t, uint32_t> findQueueFamilies(VkPhysicalDevice phys,
                                                       VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());

    uint32_t graphics = UINT32_MAX;
    uint32_t present  = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics == UINT32_MAX) {
            graphics = i;
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &presentSupport);
        if (presentSupport && present == UINT32_MAX) {
            present = i;
        }
    }
    return {graphics, present};
}

// 选 swapchain surface format
static VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats[0];
}

// 选 present mode（FIFO 必支持；优先 MAILBOX，无则 FIFO）
static VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    for (VkPresentModeKHR m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// 选 swapchain extent（与 surface capability 对齐）
static VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, int w, int h) {
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    VkExtent2D e{};
    uint32_t reqW = (uint32_t)w;
    uint32_t reqH = (uint32_t)h;
    e.width  = (reqW < caps.minImageExtent.width)  ? caps.minImageExtent.width  :
               (reqW > caps.maxImageExtent.width)  ? caps.maxImageExtent.width  : reqW;
    e.height = (reqH < caps.minImageExtent.height) ? caps.minImageExtent.height :
               (reqH > caps.maxImageExtent.height) ? caps.maxImageExtent.height : reqH;
    return e;
}

// 创建 shader module（VkShaderModule）
// 失败时返回 VK_NULL_HANDLE
static VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &ci, nullptr, &m);
    if (!checkVk(r, "vkCreateShaderModule")) return VK_NULL_HANDLE;
    return m;
}

// -----------------------------------------------------------------------------
// 单例
// -----------------------------------------------------------------------------
Renderer& Renderer::instance() {
    static Renderer r;
    return r;
}

// -----------------------------------------------------------------------------
// 共享 descriptor set layout（sampler2D @ binding=0 + UBO @ binding=1）
// 两条 pipeline 都引用此 layout
// -----------------------------------------------------------------------------
static bool createSharedDescLayout(VkDevice device,
                                   VkDescriptorSetLayout* outLayout) {
    VkDescriptorSetLayoutBinding bindings[2]{};

    // binding=0 : combined image sampler（用于采样 DXT/RGBA 纹理）
    bindings[0].binding            = 0;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].pImmutableSamplers = nullptr;

    // binding=1 : uniform buffer（每层 rect + uvRect）
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings    = bindings;

    return checkVk(vkCreateDescriptorSetLayout(device, &dslci, nullptr, outLayout),
                   "vkCreateDescriptorSetLayout (shared)");
}

// 创建 graphics pipeline（用共享 vert + 共享 frag）
// outPipeline / outLayout 接受外部传入
// 失败返回 false
static bool createGraphicsPipeline(VkDevice device,
                                   VkRenderPass renderPass,
                                   const std::vector<uint32_t>& vertSpirv,
                                   const std::vector<uint32_t>& fragSpirv,
                                   VkDescriptorSetLayout sharedDescLayout,
                                   VkPipeline* outPipeline,
                                   VkPipelineLayout* outLayout) {
    if (outPipeline) *outPipeline = VK_NULL_HANDLE;
    if (outLayout)   *outLayout   = VK_NULL_HANDLE;

    VkShaderModule vs = createShaderModule(device, vertSpirv);
    VkShaderModule fs = createShaderModule(device, fragSpirv);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (fs) vkDestroyShaderModule(device, fs, nullptr);
        return false;
    }

    // pipeline layout（只引用 shared descLayout；push constant 不用）
    VkPipelineLayoutCreateInfo plci{};
    plci.sType        = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &sharedDescLayout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineLayout(device, &plci, nullptr, &layout);
    if (!checkVk(r, "vkCreatePipelineLayout")) {
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
        return false;
    }

    // 顶点输入：2 个 vec2 attribute（aPos @ location=0, aUV @ location=1），单 vertex buffer
    VkVertexInputBindingDescription vb{};
    vb.binding    = 0;
    vb.stride     = sizeof(float) * 4;  // 2 + 2 floats
    vb.inputRate  = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[2]{};
    va[0].location = 0;
    va[0].binding  = 0;
    va[0].format   = VK_FORMAT_R32G32_SFLOAT;
    va[0].offset   = 0;
    va[1].location = 1;
    va[1].binding  = 0;
    va[1].format   = VK_FORMAT_R32G32_SFLOAT;
    va[1].offset   = sizeof(float) * 2;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = va;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType    = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable  = VK_FALSE;

    // alpha blend (SrcAlpha / OneMinusSrcAlpha) — 多层叠加必须
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable         = VK_TRUE;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;
    cb.logicOpEnable   = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyns{};
    dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 2;
    dyns.pDynamicStates    = dyn;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState      = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pDepthStencilState  = &ds;
    gpci.pColorBlendState    = &cb;
    gpci.pDynamicState       = &dyns;
    gpci.layout              = layout;
    gpci.renderPass          = renderPass;
    gpci.subpass             = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);

    // shader modules 创建完即可释放
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);

    if (!checkVk(r, "vkCreateGraphicsPipelines")) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return false;
    }

    if (outPipeline) *outPipeline = pipeline;
    if (outLayout)   *outLayout   = layout;
    return true;
}

// 创建通用 host-visible buffer（用于 vertex / index / UBO）
static bool createHostBuffer(VkDevice device,
                             VkPhysicalDevice physDevice,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             VkBuffer* outBuf,
                             VkDeviceMemory* outMem,
                             void** outMapped) {
    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = size;
    bci.usage       = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(device, &bci, nullptr, &buf);
    if (!checkVk(r, "vkCreateBuffer")) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buf, &memReq);

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = memReq.size;
    mai.memoryTypeIndex = findMemoryType(physDevice, memReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        std::fprintf(stderr, "[vulkan_renderer] 找不到 host-visible memory\n");
        vkDestroyBuffer(device, buf, nullptr);
        return false;
    }

    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, nullptr, &mem);
    if (!checkVk(r, "vkAllocateMemory")) {
        vkDestroyBuffer(device, buf, nullptr);
        return false;
    }
    r = vkBindBufferMemory(device, buf, mem, 0);
    if (!checkVk(r, "vkBindBufferMemory")) {
        vkFreeMemory(device, mem, nullptr);
        vkDestroyBuffer(device, buf, nullptr);
        return false;
    }

    if (outMapped) {
        void* mapped = nullptr;
        r = vkMapMemory(device, mem, 0, size, 0, &mapped);
        if (!checkVk(r, "vkMapMemory")) {
            vkFreeMemory(device, mem, nullptr);
            vkDestroyBuffer(device, buf, nullptr);
            return false;
        }
        *outMapped = mapped;
    }

    *outBuf = buf;
    *outMem = mem;
    return true;
}

// -----------------------------------------------------------------------------
// init
// -----------------------------------------------------------------------------
bool Renderer::init(void* hwnd, int w, int h) {
    if (initialized_) {
        std::fprintf(stderr, "[vulkan_renderer] 已初始化，忽略重复 init\n");
        return true;
    }

    // ---- 1. 创建 instance ----
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "happlayer";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName        = "happlayer";
    app.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion         = VK_API_VERSION_1_0;

    const char* exts[] = {
        "VK_KHR_surface",
#ifdef _WIN32
        "VK_KHR_win32_surface",
#endif
        "VK_EXT_debug_utils",
    };

    uint32_t layerPropCount = 0;
    vkEnumerateInstanceLayerProperties(&layerPropCount, nullptr);
    std::vector<VkLayerProperties> layerProps(layerPropCount);
    if (layerPropCount) vkEnumerateInstanceLayerProperties(&layerPropCount, layerProps.data());
    const char* valLayer = "VK_LAYER_KHRONOS_validation";
    bool hasVal = false;
    for (const auto& lp : layerProps) {
        if (std::strcmp(lp.layerName, valLayer) == 0) { hasVal = true; break; }
    }

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = hasVal ? 3u : 2u;
    ici.ppEnabledExtensionNames = exts;
    if (hasVal) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &valLayer;
        std::fprintf(stdout, "[vulkan_renderer] 启用 VK_LAYER_KHRONOS_validation\n");
    }

    if (!checkVk(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) {
        return false;
    }

    if (hasVal) {
        auto pfn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (pfn) {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            VkDebugUtilsMessengerEXT dummy = VK_NULL_HANDLE;
            pfn(instance_, &dci, nullptr, &dummy);
            (void)dummy;  // 进程退出时随 instance 一起销毁即可
        }
    }

    // ---- 2. 创建 Win32 surface ----
#ifdef _WIN32
    auto pfnCreateWin32Surface = (PFN_vkCreateWin32SurfaceKHR)
        vkGetInstanceProcAddr(instance_, "vkCreateWin32SurfaceKHR");
    if (!pfnCreateWin32Surface) {
        std::fprintf(stderr, "[vulkan_renderer] vkGetInstanceProcAddr vkCreateWin32SurfaceKHR 失败\n");
        shutdown();
        return false;
    }
    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandle(nullptr);
    sci.hwnd      = (HWND)hwnd;
    if (!checkVk(pfnCreateWin32Surface(instance_, &sci, nullptr, &surface_), "vkCreateWin32SurfaceKHR")) {
        shutdown();
        return false;
    }
#endif

    // ---- 3. 选物理设备 ----
    physDevice_ = pickPhysicalDevice(instance_, surface_);
    if (physDevice_ == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[vulkan_renderer] 找不到物理设备\n");
        shutdown();
        return false;
    }

    // ---- 4. 找队列族 ----
    auto [g, p] = findQueueFamilies(physDevice_, surface_);
    graphicsFamily_ = g;
    presentFamily_  = p;
    if (graphicsFamily_ == UINT32_MAX || presentFamily_ == UINT32_MAX) {
        std::fprintf(stderr, "[vulkan_renderer] 找不到支持 graphics+present 的队列族\n");
        shutdown();
        return false;
    }

    // ---- 5. 创建 device + queue ----
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    uint32_t uniqueFamilies[2] = {graphicsFamily_, presentFamily_};
    uint32_t nFam = (graphicsFamily_ == presentFamily_) ? 1u : 2u;
    for (uint32_t i = 0; i < nFam; ++i) {
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = uniqueFamilies[i];
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        qcis.push_back(qci);
    }
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceFeatures supportedFeats{};
    vkGetPhysicalDeviceFeatures(physDevice_, &supportedFeats);
    VkPhysicalDeviceFeatures feats{};
    feats.textureCompressionBC = supportedFeats.textureCompressionBC;
    // glslang 生成的 vert 含 gl_PerVertex{Position, PointSize, ClipDistance, CullDistance}
    feats.shaderClipDistance = supportedFeats.shaderClipDistance;
    feats.shaderCullDistance = supportedFeats.shaderCullDistance;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = sizeof(devExts) / sizeof(devExts[0]);
    dci.ppEnabledExtensionNames = devExts;
    dci.pEnabledFeatures = &feats;

    if (!checkVk(vkCreateDevice(physDevice_, &dci, nullptr, &device_), "vkCreateDevice")) {
        shutdown();
        return false;
    }
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_,  0, &presentQueue_);

    // ---- 6. swapchain ----
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDevice_, surface_, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    if (fmtCount) vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice_, surface_, &fmtCount, fmts.data());

    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    if (pmCount) vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice_, surface_, &pmCount, pms.data());

    if (fmts.empty() || pms.empty()) {
        std::fprintf(stderr, "[vulkan_renderer] swapchain formats/present-modes 数为 0\n");
        shutdown();
        return false;
    }

    VkSurfaceFormatKHR sf = chooseSwapSurfaceFormat(fmts);
    VkPresentModeKHR   pm = chooseSwapPresentMode(pms);
    VkExtent2D ext = chooseSwapExtent(caps, w, h);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR scci{};
    scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scci.surface = surface_;
    scci.minImageCount = imageCount;
    scci.imageFormat = sf.format;
    scci.imageColorSpace = sf.colorSpace;
    scci.imageExtent = ext;
    scci.imageArrayLayers = 1;
    scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t qfIndices[] = {graphicsFamily_, presentFamily_};
    if (graphicsFamily_ != presentFamily_) {
        scci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        scci.queueFamilyIndexCount = 2;
        scci.pQueueFamilyIndices = qfIndices;
    } else {
        scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    scci.preTransform = caps.currentTransform;
    scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scci.presentMode = pm;
    scci.clipped = VK_TRUE;

    if (!checkVk(vkCreateSwapchainKHR(device_, &scci, nullptr, &swapchain_), "vkCreateSwapchainKHR")) {
        shutdown();
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapImages_.data());

    swapchainFmt_ = sf.format;
    swapchainExt_ = ext;

    // ---- 7. image views ----
    swapViews_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = swapImages_[i];  // 修复：原版误用 swapViews_[i]
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swapchainFmt_;
        ivci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (!checkVk(vkCreateImageView(device_, &ivci, nullptr, &swapViews_[i]), "vkCreateImageView")) {
            shutdown();
            return false;
        }
    }

    // ---- 8. render pass ----
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = swapchainFmt_;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount  = 1;
    sub.pColorAttachments     = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &colorAtt;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;

    if (!checkVk(vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_), "vkCreateRenderPass")) {
        shutdown();
        return false;
    }

    // ---- 9. shared descriptor set layout (sampler2D + UBO) ----
    // 先创建 default sampler（所有 layer 共享）
    {
        VkSamplerCreateInfo sci{};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.anisotropyEnable = VK_FALSE;
        sci.maxAnisotropy    = 1.0f;
        sci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sci.unnormalizedCoordinates = VK_FALSE;
        sci.compareEnable    = VK_FALSE;
        sci.compareOp        = VK_COMPARE_OP_ALWAYS;
        sci.minLod = 0.0f;
        sci.maxLod = 0.0f;  // DXT/RGBA 单 mip level
        if (!checkVk(vkCreateSampler(device_, &sci, nullptr, &defaultSampler_),
                     "vkCreateSampler")) {
            shutdown();
            return false;
        }
    }
    if (!createSharedDescLayout(device_, &hapQDescLayout_)) {
        std::fprintf(stderr, "[vulkan_renderer] createSharedDescLayout 失败\n");
        shutdown();
        return false;
    }
    // 两条 pipeline 共用一个 descLayout（任务规范"两条 pipeline"通过 layout 共享 + 独立 pipeline 句柄实现）
    rgbaDescLayout_ = hapQDescLayout_;  // 同一 layout

    // ---- 10. 两条 pipeline：HAP Q + RGBA 直通 ----
    if (!createGraphicsPipeline(device_, renderPass_,
                                 vk_hapq::kSimpleVertSpirv, vk_hapq::kHapQYcocgFragSpirv,
                                 hapQDescLayout_,
                                 &hapQPipeline_, &hapQLayout_)) {
        std::fprintf(stderr, "[vulkan_renderer] HAP Q pipeline 创建失败\n");
        shutdown();
        return false;
    }
    if (!createGraphicsPipeline(device_, renderPass_,
                                 vk_hapq::kSimpleVertSpirv, vk_hapq::kRgbaFragSpirv,
                                 hapQDescLayout_,
                                 &rgbaPipeline_, &rgbaLayout_)) {
        std::fprintf(stderr, "[vulkan_renderer] RGBA pipeline 创建失败\n");
        shutdown();
        return false;
    }

    // ---- 11. 全屏 quad 顶点 + 索引缓冲 ----
    // 4 个顶点：(x, y, u, v)，NDC [-1, 1]
    // Vulkan viewport 原点在左上、Y 向下：NDC y=-1 映射到视口顶部，
    // 与 OpenGL glOrtho(0,w,h,0) + UV(0,0)=纹理左上 对齐。
    const float quadVerts[] = {
        // x,    y,    u,    v
        -1.0f, -1.0f, 0.0f, 0.0f,  // 视口左上 = UV 左上
         1.0f, -1.0f, 1.0f, 0.0f,  // 视口右上
         1.0f,  1.0f, 1.0f, 1.0f,  // 视口右下
        -1.0f,  1.0f, 0.0f, 1.0f,  // 视口左下
    };
    const uint16_t quadIdx[] = { 0, 1, 2, 0, 2, 3 };

    if (!createHostBuffer(device_, physDevice_,
                          sizeof(quadVerts),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          &quadVB_, &quadVBMem_, nullptr)) {
        std::fprintf(stderr, "[vulkan_renderer] 顶点缓冲创建失败\n");
        shutdown();
        return false;
    }
    void* vbMapped = nullptr;
    vkMapMemory(device_, quadVBMem_, 0, sizeof(quadVerts), 0, &vbMapped);
    std::memcpy(vbMapped, quadVerts, sizeof(quadVerts));
    vkUnmapMemory(device_, quadVBMem_);

    if (!createHostBuffer(device_, physDevice_,
                          sizeof(quadIdx),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          &quadIB_, &quadIBMem_, nullptr)) {
        std::fprintf(stderr, "[vulkan_renderer] 索引缓冲创建失败\n");
        shutdown();
        return false;
    }
    void* ibMapped = nullptr;
    vkMapMemory(device_, quadIBMem_, 0, sizeof(quadIdx), 0, &ibMapped);
    std::memcpy(ibMapped, quadIdx, sizeof(quadIdx));
    vkUnmapMemory(device_, quadIBMem_);

    // ---- 12. descriptor pool（kMaxLayers × frames-in-flight） ----
    const uint32_t kSetCount = kMaxLayers * kMaxFramesInFlight;
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = kSetCount;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = kSetCount;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = kSetCount;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = poolSizes;

    if (!checkVk(vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptorPool_),
                 "vkCreateDescriptorPool")) {
        shutdown();
        return false;
    }

    layerDescSets_.resize(kSetCount);
    {
        std::vector<VkDescriptorSetLayout> layouts(kSetCount, hapQDescLayout_);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = descriptorPool_;
        dsai.descriptorSetCount = kSetCount;
        dsai.pSetLayouts        = layouts.data();
        if (!checkVk(vkAllocateDescriptorSets(device_, &dsai, layerDescSets_.data()),
                     "vkAllocateDescriptorSets (prealloc)")) {
            shutdown();
            return false;
        }
    }

    // ---- 13. per-layer UBO 池（持久映射） ----
    // UBO 大小 = 32 字节（vec4 rect + vec4 uvRect，std140 对齐）
    constexpr VkDeviceSize kUBOSize = 32;
    layerUBOs_.resize(kMaxLayers);
    for (uint32_t i = 0; i < kMaxLayers; ++i) {
        if (!createHostBuffer(device_, physDevice_,
                              kUBOSize,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              &layerUBOs_[i].buffer,
                              &layerUBOs_[i].memory,
                              &layerUBOs_[i].mapped)) {
            std::fprintf(stderr, "[vulkan_renderer] UBO[%u] 创建失败\n", i);
            shutdown();
            return false;
        }
    }

    // ---- 14. framebuffers ----
    framebuffers_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageView att[] = {swapViews_[i]};
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = renderPass_;
        fci.attachmentCount = 1;
        fci.pAttachments    = att;
        fci.width           = ext.width;
        fci.height          = ext.height;
        fci.layers          = 1;
        if (!checkVk(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer")) {
            shutdown();
            return false;
        }
    }

    // ---- 15. command pool + command buffer ----
    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = graphicsFamily_;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!checkVk(vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_), "vkCreateCommandPool")) {
        shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = cmdPool_;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (!checkVk(vkAllocateCommandBuffers(device_, &cbai, &cmdBuffer_), "vkAllocateCommandBuffers")) {
        shutdown();
        return false;
    }

    // ---- 16. sync objects ----
    imageAvailable_.resize(kMaxFramesInFlight);
    renderFinished_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);
    imagesInFlight_.resize(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo sci_sema{};
    sci_sema.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fci2{};
    fci2.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci2.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (!checkVk(vkCreateSemaphore(device_, &sci_sema, nullptr, &imageAvailable_[i]),  "vkCreateSemaphore imageAvailable") ||
            !checkVk(vkCreateSemaphore(device_, &sci_sema, nullptr, &renderFinished_[i]), "vkCreateSemaphore renderFinished") ||
            !checkVk(vkCreateFence(device_, &fci2, nullptr, &inFlightFences_[i]),            "vkCreateFence inFlight")) {
            shutdown();
            return false;
        }
    }

    initialized_ = true;
    std::fprintf(stdout,
        "[vulkan_renderer] init OK (阶段 E): %ux%u, imageCount=%u, fmt=%d, "
        "HAPQ+RGBA pipelines, descSets=%u\n",
        ext.width, ext.height, imageCount, (int)sf.format, kSetCount);
    return true;
}

// -----------------------------------------------------------------------------
// drawFrame（多 layer 完整版本）
// -----------------------------------------------------------------------------
bool Renderer::drawFrame(const std::vector<DrawLayer>& layers) {
    if (!initialized_) return false;

    bool anyDirty = false;
    for (const auto& L : layers) {
        if (L.dirty && L.tex) { anyDirty = true; break; }
    }
    // 等所有 in-flight 帧结束：UBO / staging 都是单缓冲，不能和上一帧重叠
    vkWaitForFences(device_, kMaxFramesInFlight, inFlightFences_.data(), VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                        imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        std::fprintf(stderr, "[vulkan_renderer] swapchain out-of-date（窗口 resize 未处理）\n");
        return false;
    }
    if (!checkVk(r, "vkAcquireNextImageKHR")) return false;

    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

    vkResetCommandBuffer(cmdBuffer_, 0);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!checkVk(vkBeginCommandBuffer(cmdBuffer_, &cbbi), "vkBeginCommandBuffer")) return false;

    // 阶段 E：render pass 之前把 dirty 层的 staging copy 到 GPU image
    if (anyDirty) {
        for (const auto& L : layers) {
            if (L.dirty && L.tex) {
                vk_hapq::recordCopyToImage(cmdBuffer_, *L.tex);
                L.tex->layoutUndefined = false;
            }
        }
    }

    VkClearValue cv{};
    cv.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass      = renderPass_;
    rpbi.framebuffer     = framebuffers_[imageIndex];
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = swapchainExt_;
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &cv;

    vkCmdBeginRenderPass(cmdBuffer_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkDeviceSize vOffset = 0;
    vkCmdBindVertexBuffers(cmdBuffer_, 0, 1, &quadVB_, &vOffset);
    vkCmdBindIndexBuffer(cmdBuffer_, quadIB_, 0, VK_INDEX_TYPE_UINT16);

    struct UBOData {
        float rectX, rectY, rectW, rectH;
        float uvX0, uvY0, uvX1, uvY1;
    };

    uint32_t layersDrawn = 0;
    for (uint32_t i = 0; i < layers.size() && i < kMaxLayers; ++i) {
        const DrawLayer& L = layers[i];
        if (L.imageView == VK_NULL_HANDLE) continue;
        if (L.rectW <= 0.5f || L.rectH <= 0.5f) continue;

        UBOData ubo;
        ubo.rectX = L.rectX; ubo.rectY = L.rectY;
        ubo.rectW = L.rectW; ubo.rectH = L.rectH;
        ubo.uvX0  = L.uvX0;  ubo.uvY0  = L.uvY0;
        ubo.uvX1  = L.uvX1;  ubo.uvY1  = L.uvY1;
        std::memcpy(layerUBOs_[i].mapped, &ubo, sizeof(ubo));

        const uint32_t dsIndex = currentFrame_ * kMaxLayers + i;
        VkDescriptorSet ds = layerDescSets_[dsIndex];

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = defaultSampler_;
        imgInfo.imageView   = L.imageView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = layerUBOs_[i].buffer;
        bufInfo.offset = 0;
        bufInfo.range  = sizeof(UBOData);

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = ds;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &imgInfo;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = ds;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

        VkPipeline pipe = L.useHapQ ? hapQPipeline_ : rgbaPipeline_;
        vkCmdBindPipeline(cmdBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmdBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 L.useHapQ ? hapQLayout_ : rgbaLayout_,
                                 0, 1, &ds, 0, nullptr);

        // 每层 viewport + scissor = 图层 rect（像素，左上原点）
        VkViewport vp{};
        vp.x        = L.rectX;
        vp.y        = L.rectY;
        vp.width    = L.rectW;
        vp.height   = L.rectH;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmdBuffer_, 0, 1, &vp);

        int32_t sx = (int32_t)std::max(0.0f, L.rectX);
        int32_t sy = (int32_t)std::max(0.0f, L.rectY);
        int32_t x1 = (int32_t)std::min((float)swapchainExt_.width,  L.rectX + L.rectW);
        int32_t y1 = (int32_t)std::min((float)swapchainExt_.height, L.rectY + L.rectH);
        if (x1 <= sx || y1 <= sy) continue;
        VkRect2D sc{};
        sc.offset = { sx, sy };
        sc.extent = { (uint32_t)(x1 - sx), (uint32_t)(y1 - sy) };
        vkCmdSetScissor(cmdBuffer_, 0, 1, &sc);

        vkCmdDrawIndexed(cmdBuffer_, 6, 1, 0, 0, 0);
        layersDrawn++;
    }

    vkCmdEndRenderPass(cmdBuffer_);

    if (!checkVk(vkEndCommandBuffer(cmdBuffer_), "vkEndCommandBuffer")) return false;

    VkSemaphore waitSemas[] = {imageAvailable_[currentFrame_]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore sigSemas[] = {renderFinished_[currentFrame_]};

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = waitSemas;
    si.pWaitDstStageMask    = waitStages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmdBuffer_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = sigSemas;

    if (!checkVk(vkQueueSubmit(graphicsQueue_, 1, &si, inFlightFences_[currentFrame_]), "vkQueueSubmit")) {
        return false;
    }

    VkSwapchainKHR scs[] = {swapchain_};
    VkPresentInfoKHR pi{};
    pi.sType               = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount  = 1;
    pi.pWaitSemaphores     = sigSemas;
    pi.swapchainCount      = 1;
    pi.pSwapchains         = scs;
    pi.pImageIndices       = &imageIndex;

    r = vkQueuePresentKHR(presentQueue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[vulkan_renderer] present 后 swapchain out-of-date/suboptimal\n");
        return false;
    }
    if (!checkVk(r, "vkQueuePresentKHR")) return false;

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    (void)layersDrawn;
    return true;
}

// -----------------------------------------------------------------------------
// waitIdle
// -----------------------------------------------------------------------------
void Renderer::waitIdle() {
    if (!initialized_) return;
    vkDeviceWaitIdle(device_);
}

// -----------------------------------------------------------------------------
// shutdown
// -----------------------------------------------------------------------------
void Renderer::shutdown() {
    if (!initialized_ && instance_ == VK_NULL_HANDLE && device_ == VK_NULL_HANDLE) {
        return;  // 幂等
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);

        // 销毁 sync
        for (auto s : imageAvailable_)  if (s) vkDestroySemaphore(device_, s, nullptr);
        for (auto s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
        for (auto f : inFlightFences_) if (f) vkDestroyFence(device_, f, nullptr);
        imageAvailable_.clear();
        renderFinished_.clear();
        inFlightFences_.clear();
        imagesInFlight_.clear();

        // 销毁 command
        if (cmdBuffer_) {
            vkFreeCommandBuffers(device_, cmdPool_, 1, &cmdBuffer_);
            cmdBuffer_ = VK_NULL_HANDLE;
        }
        if (cmdPool_) {
            vkDestroyCommandPool(device_, cmdPool_, nullptr);
            cmdPool_ = VK_NULL_HANDLE;
        }

        // 销毁 framebuffers
        for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        framebuffers_.clear();

        // 销毁 UBO 池（先 unmap，再 free mem，最后 destroy buffer）
        for (auto& u : layerUBOs_) {
            if (u.mapped && u.memory) vkUnmapMemory(device_, u.memory);
            if (u.buffer)  vkDestroyBuffer(device_, u.buffer, nullptr);
            if (u.memory)  vkFreeMemory(device_, u.memory, nullptr);
        }
        layerUBOs_.clear();

        // 销毁 descriptor pool（会自动释放其分配的所有 descriptor set）
        if (descriptorPool_) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }
        layerDescSets_.clear();

        // 销毁全屏 quad 顶点/索引
        if (quadVB_)  { vkDestroyBuffer(device_, quadVB_,  nullptr); quadVB_  = VK_NULL_HANDLE; }
        if (quadVBMem_) { vkFreeMemory(device_, quadVBMem_, nullptr);  quadVBMem_ = VK_NULL_HANDLE; }
        if (quadIB_)  { vkDestroyBuffer(device_, quadIB_,  nullptr); quadIB_  = VK_NULL_HANDLE; }
        if (quadIBMem_) { vkFreeMemory(device_, quadIBMem_, nullptr);  quadIBMem_ = VK_NULL_HANDLE; }

        // 销毁两条 pipeline（先销毁 pipeline，再销毁 layout，再销毁 descLayout）
        if (hapQPipeline_) { vkDestroyPipeline(device_, hapQPipeline_, nullptr); hapQPipeline_ = VK_NULL_HANDLE; }
        if (hapQLayout_)   { vkDestroyPipelineLayout(device_, hapQLayout_, nullptr); hapQLayout_ = VK_NULL_HANDLE; }
        if (rgbaPipeline_) { vkDestroyPipeline(device_, rgbaPipeline_, nullptr); rgbaPipeline_ = VK_NULL_HANDLE; }
        if (rgbaLayout_)   { vkDestroyPipelineLayout(device_, rgbaLayout_, nullptr); rgbaLayout_ = VK_NULL_HANDLE; }
        if (hapQDescLayout_) {
            // hapQDescLayout_ 和 rgbaDescLayout_ 是同一指针，重复 destroy 会触发 validation layer warning
            // 安全做法：先置 rgbaDescLayout_ 为 null，再 destroy hapQDescLayout_
            rgbaDescLayout_ = VK_NULL_HANDLE;
            vkDestroyDescriptorSetLayout(device_, hapQDescLayout_, nullptr);
            hapQDescLayout_ = VK_NULL_HANDLE;
        }
        // 销毁 default sampler
        if (defaultSampler_) {
            vkDestroySampler(device_, defaultSampler_, nullptr);
            defaultSampler_ = VK_NULL_HANDLE;
        }

        // 销毁 render pass
        if (renderPass_) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }

        // 销毁 image views + swapchain
        for (auto v : swapViews_) if (v) vkDestroyImageView(device_, v, nullptr);
        swapViews_.clear();
        swapImages_.clear();
        if (swapchain_) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }

        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        if (surface_) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    physDevice_    = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    presentQueue_  = VK_NULL_HANDLE;
    graphicsFamily_ = UINT32_MAX;
    presentFamily_  = UINT32_MAX;
    swapchainFmt_ = VK_FORMAT_UNDEFINED;
    swapchainExt_ = {};
    currentFrame_ = 0;
    initialized_ = false;
}

} // namespace vk_render
