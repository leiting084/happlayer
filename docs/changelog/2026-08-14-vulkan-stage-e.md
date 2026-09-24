# 2026-08-14 Vulkan 阶段 E 接通

> 项目：happlayer
> 类型：里程碑（可选加速路径从「编译过」变成「真能播」）

## 做了什么
- 逐帧 DXT/RGBA 上传（持久 staging + draw 前 copy）
- `HAPPLAYER_USE_VULKAN=1` 时 `GLFW_NO_API`
- 手写可用 SPIR-V（简单 vert + RGBA 直通 + HAP Q YCoCg）
- descriptor set 预分配；每层 viewport/scissor

## 实测（640×360，`--max-fps 30`）
| 路径 | 素材 | FPS |
|------|------|-----|
| OpenGL | H.264 | ~22 |
| Vulkan | H.264 | 30 锁满 |
| Vulkan | HAP | 30 锁满 |

## 反哺点
1. GLFW 窗口上挂 Vulkan **必须** `GLFW_NO_API`，否则 Win32 surface Access Violation → 内部规则 `glfw-vulkan-no-api`
2. `vkCreateShaderModule` 成功不能当验收 → 内部规则 `vulkan-shader-module-not-pipeline`
3. 逐帧纹理用持久 staging，copy 进同一 CB → pattern `patterns/vulkan-persistent-staging-upload/`
4. 可选路径「编译过」≠「跑过」：阶段 C/D 文档超前，阶段 E 才第一次真跑

## 未做
- swapchain resize
- Vulkan + NVDEC 零拷贝
- 双缓冲 staging（当前等全部 in-flight fence）
