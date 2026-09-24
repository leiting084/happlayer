// vulkan_hapq.cpp — Vulkan 阶段 A+B 实现
//
// 阶段 A：HAP Q shader pipeline（YCoCg DXT5 → RGB）
//   - SPIR-V 字节码嵌入（glslang -V 离线编译生成）
//   - shader module + descriptor set layout + pipeline layout + graphics pipeline
//
// 阶段 B：DXT 压缩纹理上传
//   - staging buffer → vkCmdCopyBufferToImage → barrier → shader read
//
// API 兼容性：Vulkan 1.0（不依赖 1.1/1.2），调用方需自行创建 VkInstance/VkDevice。
// 仅编译不调用：happlayer 默认仍用 OpenGL，本模块通过 HAPPLAYER_HAS_VULKAN 宏启用。

#include "vulkan_hapq.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// 关闭 MSVC 安全警告：sprintf 不安全 → 用 snprintf
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)  // 'sprintf': This function may be unsafe
#endif

namespace vk_hapq {

// -----------------------------------------------------------------------------
// SPIR-V 字节码（glslang -V 自动生成）
// -----------------------------------------------------------------------------
//
// 对应 GLSL：
//   vert:  #version 450, layout(location=0) in vec2 aPos, layout(location=1) in vec2 aUV
//   frag:  #version 450, layout(binding=0) uniform sampler2D uTex
//                    YCoCg DXT5 → RGB (HAP Q 不带 alpha)
//
// vert = 240 words / 960 bytes
// frag = 306 words / 1224 bytes

const std::vector<uint32_t> kHapQVertSpirv = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000001fu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000012u, 0x0000001cu,
    0x0000001du, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu,
    0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u,
    0x746e696fu, 0x657a6953u, 0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
    0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u,
    0x0065636eu, 0x00030005u, 0x0000000du, 0x00000000u, 0x00040005u, 0x00000012u, 0x736f5061u, 0x00000000u,
    0x00030005u, 0x0000001cu, 0x00565576u, 0x00030005u, 0x0000001du, 0x00565561u, 0x00030047u, 0x0000000bu,
    0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu,
    0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u,
    0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x00000012u, 0x0000001eu,
    0x00000000u, 0x00040047u, 0x0000001cu, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000001du, 0x0000001eu,
    0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u,
    0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u,
    0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u,
    0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u,
    0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040017u,
    0x00000010u, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000011u, 0x00000001u, 0x00000010u, 0x0004003bu,
    0x00000011u, 0x00000012u, 0x00000001u, 0x0004002bu, 0x00000006u, 0x00000014u, 0x00000000u, 0x0004002bu,
    0x00000006u, 0x00000015u, 0x3f800000u, 0x00040020u, 0x00000019u, 0x00000003u, 0x00000007u, 0x00040020u,
    0x0000001bu, 0x00000003u, 0x00000010u, 0x0004003bu, 0x0000001bu, 0x0000001cu, 0x00000003u, 0x0004003bu,
    0x00000011u, 0x0000001du, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u, 0x0004003du, 0x00000010u, 0x00000013u, 0x00000012u, 0x00050051u, 0x00000006u,
    0x00000016u, 0x00000013u, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000017u, 0x00000013u, 0x00000001u,
    0x00070050u, 0x00000007u, 0x00000018u, 0x00000016u, 0x00000017u, 0x00000014u, 0x00000015u, 0x00050041u,
    0x00000019u, 0x0000001au, 0x0000000du, 0x0000000fu, 0x0003003eu, 0x0000001au, 0x00000018u, 0x0004003du,
    0x00000010u, 0x0000001eu, 0x0000001du, 0x0003003eu, 0x0000001cu, 0x0000001eu, 0x000100fdu, 0x00010038u,
};

const std::vector<uint32_t> kHapQFragSpirv = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000035u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000011u, 0x00000023u, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du,
    0x00000000u, 0x00030005u, 0x00000009u, 0x00006379u, 0x00040005u, 0x0000000du, 0x78655475u, 0x00000000u,
    0x00030005u, 0x00000011u, 0x00565576u, 0x00030005u, 0x00000015u, 0x00000059u, 0x00030005u, 0x0000001au,
    0x00006f43u, 0x00030005u, 0x0000001eu, 0x00006743u, 0x00050005u, 0x00000023u, 0x67617266u, 0x6f6c6f43u,
    0x00000072u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000000du, 0x00000022u,
    0x00000000u, 0x00040047u, 0x00000011u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000023u, 0x0000001eu,
    0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000007u,
    0x00000007u, 0x00090019u, 0x0000000au, 0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x0003001bu, 0x0000000bu, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000000u,
    0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000000u, 0x00040017u, 0x0000000fu, 0x00000006u,
    0x00000002u, 0x00040020u, 0x00000010u, 0x00000001u, 0x0000000fu, 0x0004003bu, 0x00000010u, 0x00000011u,
    0x00000001u, 0x00040020u, 0x00000014u, 0x00000007u, 0x00000006u, 0x00040015u, 0x00000016u, 0x00000020u,
    0x00000000u, 0x0004002bu, 0x00000016u, 0x00000017u, 0x00000000u, 0x0004002bu, 0x00000016u, 0x0000001bu,
    0x00000001u, 0x0004002bu, 0x00000016u, 0x0000001fu, 0x00000002u, 0x00040020u, 0x00000022u, 0x00000003u,
    0x00000007u, 0x0004003bu, 0x00000022u, 0x00000023u, 0x00000003u, 0x0004002bu, 0x00000016u, 0x00000031u,
    0x00000003u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x0004003bu, 0x00000008u, 0x00000009u, 0x00000007u, 0x0004003bu, 0x00000014u, 0x00000015u, 0x00000007u,
    0x0004003bu, 0x00000014u, 0x0000001au, 0x00000007u, 0x0004003bu, 0x00000014u, 0x0000001eu, 0x00000007u,
    0x0004003du, 0x0000000bu, 0x0000000eu, 0x0000000du, 0x0004003du, 0x0000000fu, 0x00000012u, 0x00000011u,
    0x00050057u, 0x00000007u, 0x00000013u, 0x0000000eu, 0x00000012u, 0x0003003eu, 0x00000009u, 0x00000013u,
    0x00050041u, 0x00000014u, 0x00000018u, 0x00000009u, 0x00000017u, 0x0004003du, 0x00000006u, 0x00000019u,
    0x00000018u, 0x0003003eu, 0x00000015u, 0x00000019u, 0x00050041u, 0x00000014u, 0x0000001cu, 0x00000009u,
    0x0000001bu, 0x0004003du, 0x00000006u, 0x0000001du, 0x0000001cu, 0x0003003eu, 0x0000001au, 0x0000001du,
    0x00050041u, 0x00000014u, 0x00000020u, 0x00000009u, 0x0000001fu, 0x0004003du, 0x00000006u, 0x00000021u,
    0x00000020u, 0x0003003eu, 0x0000001eu, 0x00000021u, 0x0004003du, 0x00000006u, 0x00000024u, 0x00000015u,
    0x0004003du, 0x00000006u, 0x00000025u, 0x0000001au, 0x00050083u, 0x00000006u, 0x00000026u, 0x00000024u,
    0x00000025u, 0x0004003du, 0x00000006u, 0x00000027u, 0x0000001eu, 0x00050083u, 0x00000006u, 0x00000028u,
    0x00000027u, 0x0004003du, 0x00000006u, 0x00000029u, 0x00000015u, 0x0004003du, 0x00000006u, 0x0000002au,
    0x0000001eu, 0x00050081u, 0x00000006u, 0x0000002bu, 0x00000029u, 0x0000002au, 0x0004003du, 0x00000006u,
    0x0000002cu, 0x00000015u, 0x0004003du, 0x00000006u, 0x0000002du, 0x0000001au, 0x00050081u, 0x00000006u,
    0x0000002eu, 0x0000002cu, 0x0000002du,     0x0004003du, 0x00000006u, 0x0000002fu, 0x0000001eu, 0x00050083u,
    0x00000006u, 0x00000030u, 0x0000002eu, 0x0000002fu, 0x00050041u, 0x00000014u, 0x00000032u, 0x00000009u,
    0x00000031u, 0x0004003du, 0x00000006u, 0x00000033u, 0x00000032u, 0x00070050u, 0x00000007u, 0x00000034u,
    0x00000028u, 0x0000002bu, 0x00000030u, 0x00000033u, 0x0003003eu, 0x00000023u, 0x00000034u, 0x000100fdu,
    0x00010038u,
};

// RGBA / DXT1 / Hap Alpha 直通 fragment shader（scripts/gen_rgba_spirv.py）
const std::vector<uint32_t> kRgbaFragSpirv = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000014u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000010u, 0x00000009u, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00050005u, 0x00000009u,
    0x67617266u, 0x6f6c6f43u, 0x00000072u, 0x00040005u, 0x0000000du, 0x78655475u, 0x00000000u, 0x00030005u,
    0x00000010u, 0x00565576u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000du,
    0x00000022u, 0x00000000u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000010u,
    0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
    0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00090019u, 0x0000000au,
    0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu,
    0x0000000bu, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000000u, 0x0000000bu, 0x0004003bu, 0x0000000cu,
    0x0000000du, 0x00000000u, 0x00040017u, 0x0000000eu, 0x00000006u, 0x00000002u, 0x00040020u, 0x0000000fu,
    0x00000001u, 0x0000000eu, 0x0004003bu, 0x0000000fu, 0x00000010u, 0x00000001u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000bu, 0x00000011u,
    0x0000000du, 0x0004003du, 0x0000000eu, 0x00000012u, 0x00000010u, 0x00050057u, 0x00000007u, 0x00000013u,
    0x00000011u, 0x00000012u, 0x0003003eu, 0x00000009u, 0x00000013u, 0x000100fdu, 0x00010038u,
};

// 简易 vertex shader（无 gl_PerVertex ClipDistance，scripts/gen_vert_spirv.py）
const std::vector<uint32_t> kSimpleVertSpirv = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000017u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000au, 0x0000000bu, 0x0000000du,
    0x0000000fu, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u, 0x0000000au, 0x736f5061u,
    0x00000000u, 0x00030005u, 0x0000000bu, 0x00565561u, 0x00030005u, 0x0000000du, 0x00565576u, 0x00050005u,
    0x0000000fu, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00040047u, 0x0000000au, 0x0000001eu, 0x00000000u,
    0x00040047u, 0x0000000bu, 0x0000001eu, 0x00000001u, 0x00040047u, 0x0000000du, 0x0000001eu, 0x00000000u,
    0x00040047u, 0x0000000fu, 0x0000000bu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000002u,
    0x00040017u, 0x00000008u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000009u, 0x00000001u, 0x00000007u,
    0x0004003bu, 0x00000009u, 0x0000000au, 0x00000001u, 0x0004003bu, 0x00000009u, 0x0000000bu, 0x00000001u,
    0x00040020u, 0x0000000cu, 0x00000003u, 0x00000007u, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u,
    0x00040020u, 0x0000000eu, 0x00000003u, 0x00000008u, 0x0004003bu, 0x0000000eu, 0x0000000fu, 0x00000003u,
    0x0004002bu, 0x00000006u, 0x00000013u, 0x00000000u, 0x0004002bu, 0x00000006u, 0x00000014u, 0x3f800000u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du,
    0x00000007u, 0x00000010u, 0x0000000au, 0x00050051u, 0x00000006u, 0x00000011u, 0x00000010u, 0x00000000u,
    0x00050051u, 0x00000006u, 0x00000012u, 0x00000010u, 0x00000001u, 0x00070050u, 0x00000008u, 0x00000015u,
    0x00000011u, 0x00000012u, 0x00000013u, 0x00000014u, 0x0003003eu, 0x0000000fu, 0x00000015u, 0x0004003du,
    0x00000007u, 0x00000016u, 0x0000000bu, 0x0003003eu, 0x0000000du, 0x00000016u, 0x000100fdu, 0x00010038u,
};

const std::vector<uint32_t> kHapQYcocgFragSpirv = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000001eu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000010u, 0x00000009u, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00050005u, 0x00000009u,
    0x67617266u, 0x6f6c6f43u, 0x00000072u, 0x00040005u, 0x0000000du, 0x78655475u, 0x00000000u, 0x00030005u,
    0x00000010u, 0x00565576u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000du,
    0x00000022u, 0x00000000u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000010u,
    0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
    0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00090019u, 0x0000000au,
    0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu,
    0x0000000bu, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000000u, 0x0000000bu, 0x0004003bu, 0x0000000cu,
    0x0000000du, 0x00000000u, 0x00040017u, 0x0000000eu, 0x00000006u, 0x00000002u, 0x00040020u, 0x0000000fu,
    0x00000001u, 0x0000000eu, 0x0004003bu, 0x0000000fu, 0x00000010u, 0x00000001u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000bu, 0x00000011u,
    0x0000000du, 0x0004003du, 0x0000000eu, 0x00000012u, 0x00000010u, 0x00050057u, 0x00000007u, 0x00000013u,
    0x00000011u, 0x00000012u, 0x00050051u, 0x00000006u, 0x00000014u, 0x00000013u, 0x00000000u, 0x00050051u,
    0x00000006u, 0x00000015u, 0x00000013u, 0x00000001u, 0x00050051u, 0x00000006u, 0x00000016u, 0x00000013u,
    0x00000002u, 0x00050051u, 0x00000006u, 0x00000017u, 0x00000013u, 0x00000003u, 0x00050083u, 0x00000006u,
    0x00000018u, 0x00000014u, 0x00000015u, 0x00050083u, 0x00000006u, 0x00000019u, 0x00000018u, 0x00000016u,
    0x00050081u, 0x00000006u, 0x0000001au, 0x00000014u, 0x00000016u, 0x00050081u, 0x00000006u, 0x0000001bu,
    0x00000014u, 0x00000015u, 0x00050083u, 0x00000006u, 0x0000001cu, 0x0000001bu, 0x00000016u, 0x00070050u,
    0x00000007u, 0x0000001du, 0x00000019u, 0x0000001au, 0x0000001cu, 0x00000017u, 0x0003003eu, 0x00000009u,
    0x0000001du, 0x000100fdu, 0x00010038u,
};

// -----------------------------------------------------------------------------
// 内部辅助函数
// -----------------------------------------------------------------------------

// 检查 VkResult 是否成功，否则输出错误并返回 false
static bool checkVk(VkResult r, const char* op) {
    if (r == VK_SUCCESS) return true;
    std::fprintf(stderr, "[vulkan_hapq] %s 失败: VkResult=%d\n", op, (int)r);
    return false;
}

// 查找支持给定 memory type 的 memory type index
//   bits: VkPhysicalDeviceMemoryProperties.memoryTypeBits 位掩码
//   props: 需要的属性（如 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT）
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
    return UINT32_MAX;  // 无匹配
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
// createHapQPipeline：阶段 A 主体
// -----------------------------------------------------------------------------
VkPipeline createHapQPipeline(VkDevice device,
                              VkRenderPass renderPass,
                              VkPipelineLayout* outLayout,
                              VkDescriptorSetLayout* outDescLayout) {
    if (outLayout) *outLayout = VK_NULL_HANDLE;
    if (outDescLayout) *outDescLayout = VK_NULL_HANDLE;

    // ---- 1. 创建 shader modules ----
    VkShaderModule vs = createShaderModule(device, kHapQVertSpirv);
    VkShaderModule fs = createShaderModule(device, kHapQFragSpirv);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (fs) vkDestroyShaderModule(device, fs, nullptr);
        return VK_NULL_HANDLE;
    }

    // ---- 2. 创建 descriptor set layout（1 个 immutable sampler 不需要；用 uniform sampler2D） ----
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;

    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorSetLayout(device, &dslci, nullptr, &descLayout);
    if (!checkVk(r, "vkCreateDescriptorSetLayout")) {
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
        return VK_NULL_HANDLE;
    }

    // ---- 3. 创建 pipeline layout ----
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descLayout;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(device, &plci, nullptr, &layout);
    if (!checkVk(r, "vkCreatePipelineLayout")) {
        vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
        return VK_NULL_HANDLE;
    }

    // ---- 4. 准备 graphics pipeline 创建信息 ----
    // 顶点输入：2 个 vec2 attribute（aPos @ location=0, aUV @ location=1），单 vertex buffer
    VkVertexInputBindingDescription vb{};
    vb.binding = 0;
    vb.stride = sizeof(float) * 4;  // 2 + 2 floats
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[2]{};
    va[0].location = 0;
    va[0].binding = 0;
    va[0].format = VK_FORMAT_R32G32_SFLOAT;
    va[0].offset = 0;
    va[1].location = 1;
    va[1].binding = 0;
    va[1].format = VK_FORMAT_R32G32_SFLOAT;
    va[1].offset = sizeof(float) * 2;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = va;

    // 输入装配：triangle list
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;

    // 视口 / 裁剪（动态）
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    // 光栅化：cull none，front face CCW
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;

    // 多采样
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;

    // 混合：alpha blend (SrcAlpha / OneMinusSrcAlpha)
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    cb.logicOpEnable = VK_FALSE;

    // 深度 / 模板：disable（仅颜色混合）
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    // 动态状态：viewport + scissor
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyns{};
    dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 2;
    dyns.pDynamicStates = dyn;

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // 完整 graphics pipeline
    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyns;
    gpci.layout = layout;
    gpci.renderPass = renderPass;
    gpci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);

    // shader modules 创建完即可释放（pipeline 持有它们）
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);

    if (!checkVk(r, "vkCreateGraphicsPipelines")) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
        return VK_NULL_HANDLE;
    }

    if (outLayout) *outLayout = layout;
    if (outDescLayout) *outDescLayout = descLayout;
    return pipeline;
}

// -----------------------------------------------------------------------------
// destroyHapQPipeline：幂等销毁
// -----------------------------------------------------------------------------
void destroyHapQPipeline(VkDevice device,
                         VkPipeline pipeline,
                         VkPipelineLayout layout,
                         VkDescriptorSetLayout descLayout) {
    if (pipeline)   vkDestroyPipeline(device, pipeline, nullptr);
    if (layout)     vkDestroyPipelineLayout(device, layout, nullptr);
    if (descLayout) vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
}

// -----------------------------------------------------------------------------
// 阶段 E：createTexture / updateTexture / recordCopyToImage
// -----------------------------------------------------------------------------

static bool formatSupported(VkFormat format) {
    return format == VK_FORMAT_BC1_RGB_UNORM_BLOCK ||
           format == VK_FORMAT_BC3_UNORM_BLOCK ||
           format == VK_FORMAT_R8G8B8A8_UNORM;
}

DXTImage createTexture(VkDevice device,
                       VkPhysicalDevice physDevice,
                       const void* data,
                       size_t dataSize,
                       uint32_t width,
                       uint32_t height,
                       VkFormat format) {
    DXTImage out{};
    if (device == VK_NULL_HANDLE || physDevice == VK_NULL_HANDLE ||
        data == nullptr || dataSize == 0 || width == 0 || height == 0) {
        return out;
    }
    if (!formatSupported(format)) {
        std::fprintf(stderr, "[vulkan_hapq] createTexture: 不支持 format=%d\n", (int)format);
        return out;
    }

    VkResult r;

    // ---- 1. 持久 staging buffer（host visible + 保持 mapped） ----
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = dataSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    r = vkCreateBuffer(device, &bci, nullptr, &stagingBuf);
    if (!checkVk(r, "vkCreateBuffer(staging)")) return out;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, stagingBuf, &memReq);

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(physDevice, memReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        std::fprintf(stderr, "[vulkan_hapq] createTexture: 找不到 host visible memory\n");
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, nullptr, &stagingMem);
    if (!checkVk(r, "vkAllocateMemory(staging)")) {
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    r = vkBindBufferMemory(device, stagingBuf, stagingMem, 0);
    if (!checkVk(r, "vkBindBufferMemory(staging)")) {
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    void* mapped = nullptr;
    r = vkMapMemory(device, stagingMem, 0, memReq.size, 0, &mapped);
    if (!checkVk(r, "vkMapMemory(staging)")) {
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }
    std::memcpy(mapped, data, dataSize);

    // ---- 2. 目标 image（optimal + transfer dst + sampled） ----
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    r = vkCreateImage(device, &ici, nullptr, &image);
    if (!checkVk(r, "vkCreateImage")) {
        vkUnmapMemory(device, stagingMem);
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    vkGetImageMemoryRequirements(device, image, &memReq);
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(physDevice, memReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        std::fprintf(stderr, "[vulkan_hapq] createTexture: 找不到 device local memory\n");
        vkDestroyImage(device, image, nullptr);
        vkUnmapMemory(device, stagingMem);
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, nullptr, &imageMem);
    if (!checkVk(r, "vkAllocateMemory(image)")) {
        vkDestroyImage(device, image, nullptr);
        vkUnmapMemory(device, stagingMem);
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    r = vkBindImageMemory(device, image, imageMem, 0);
    if (!checkVk(r, "vkBindImageMemory")) {
        vkFreeMemory(device, imageMem, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkUnmapMemory(device, stagingMem);
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    // ---- 3. image view ----
    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    r = vkCreateImageView(device, &ivci, nullptr, &view);
    if (!checkVk(r, "vkCreateImageView")) {
        vkFreeMemory(device, imageMem, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkUnmapMemory(device, stagingMem);
        vkFreeMemory(device, stagingMem, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return out;
    }

    out.image = image;
    out.memory = imageMem;
    out.view = view;
    out.width = width;
    out.height = height;
    out.format = format;
    out.dataSize = dataSize;
    out.stagingBuf = stagingBuf;
    out.stagingMem = stagingMem;
    out.stagingMapped = mapped;
    out.stagingCap = dataSize;
    out.layoutUndefined = true;
    return out;
}

bool updateTexture(DXTImage& img, const void* data, size_t dataSize) {
    if (img.image == VK_NULL_HANDLE || img.stagingMapped == nullptr ||
        data == nullptr || dataSize == 0) {
        return false;
    }
    if (dataSize > img.stagingCap) {
        std::fprintf(stderr, "[vulkan_hapq] updateTexture: dataSize %zu > stagingCap %zu\n",
                     dataSize, img.stagingCap);
        return false;
    }
    std::memcpy(img.stagingMapped, data, dataSize);
    img.dataSize = dataSize;
    return true;
}

void recordCopyToImage(VkCommandBuffer cmd, const DXTImage& img) {
    if (cmd == VK_NULL_HANDLE || img.image == VK_NULL_HANDLE ||
        img.stagingBuf == VK_NULL_HANDLE) {
        return;
    }

    VkImageMemoryBarrier barrier1{};
    barrier1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier1.srcAccessMask = img.layoutUndefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    barrier1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier1.oldLayout = img.layoutUndefined
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.image = img.image;
    barrier1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        img.layoutUndefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier1);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = { 0, 0, 0 };
    copy.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyBufferToImage(cmd, img.stagingBuf, img.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier barrier2{};
    barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.image = img.image;
    barrier2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier2);
}

DXTImage uploadDXT(VkDevice device,
                   VkPhysicalDevice physDevice,
                   VkCommandPool /*cmdPool*/,
                   VkQueue /*queue*/,
                   const void* data,
                   size_t dataSize,
                   uint32_t width,
                   uint32_t height,
                   VkFormat format) {
    return createTexture(device, physDevice, data, dataSize, width, height, format);
}

void destroyDXT(VkDevice device, DXTImage& img) {
    if (device != VK_NULL_HANDLE) {
        if (img.stagingMapped && img.stagingMem) {
            vkUnmapMemory(device, img.stagingMem);
        }
        if (img.stagingBuf) vkDestroyBuffer(device, img.stagingBuf, nullptr);
        if (img.stagingMem) vkFreeMemory(device, img.stagingMem, nullptr);
        if (img.view)       vkDestroyImageView(device, img.view, nullptr);
        if (img.image)      vkDestroyImage(device, img.image, nullptr);
        if (img.memory)     vkFreeMemory(device, img.memory, nullptr);
    }
    img = DXTImage{};
}

} // namespace vk_hapq

#ifdef _MSC_VER
#pragma warning(pop)
#endif