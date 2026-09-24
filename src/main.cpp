// happlayer — 多层 HAP 透明视频播放器（队列版，多线程解码）
//
// 一个队列 = 一组图层，层按配置顺序从下往上叠，各自有开始秒与循环开关。
// 队列内播完的层自动退出；当队列中所有「不循环」的层都播完，队列结束，
// 自动切到下一队列；最后一个队列播完回到第一个队列循环。
//
// 架构：每层一个解码线程（读盘 + snappy 解压 → 环形预读缓冲），
//       主线程只做纹理上传与合成，UDP 控制每帧轮询。
//
// 用法: happlayer [config] [--bench 秒] [--port 端口]
//                [--size 宽x高] [--pos x,y] [--monitor N] [--scale 百分比] [--fullscreen] [--borderless]
//                [--json] [--log-level error|warn|info|debug]
//   config 默认 layers.txt：
//     [window]           ← 可选，窗口/显示设置（key=value，命令行参数优先）
//     [queue]            ← 每个 [queue] 开始一个新队列
//     开始秒|循环(0/1)|视频路径
// 按键: 空格=播放/暂停  R=重播当前队列  F=全屏切换  Esc=退出

// 让 Windows 混合显卡机器优先用独显跑 OpenGL（NVIDIA Optimus / AMD PowerXpress）
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
// GL/glext.h 在 Windows 标准 OpenGL 包里没有，需要自己 typedef 函数指针
// 见下方 typedef 区
#include <dwmapi.h>
#include <mmsystem.h>   // timeBeginPeriod：frame pacing 的 sleep_for 需要 1ms 计时器精度
#include <shlobj.h>      // 小项 3：SHGetFolderPathW(CSIDL_APPDATA) 拿 APPDATA 路径
// 开机自启三件套 — 防休眠：SetThreadExecutionState 在 windows.h，
// 必须在 winsock2 之后 include（windows.h 头会跳过 winsock1 因为 _WINSOCK2_H_ 已定义）
#include <windows.h>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>      // P2.3: NVDEC hw_device_ctx / hwframe
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>     // P0.3 v2: std::shared_mutex / std::shared_lock
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hapdecode.h"
#include "audio.h"

#ifdef HAPPLAYER_HAS_VULKAN
// Vulkan 阶段 A+B 模块（HAP Q shader pipeline + DXT 压缩纹理上传）
// 当前默认未调用，仅作可切换路径的编译验证。
// 启用方式：CMake 检测 deps/vulkan/{include,lib} 自动打开 HAPPLAYER_HAS_VULKAN=1。
#include "vulkan_hapq.h"
// Vulkan 阶段 C 模块（单 surface 简化渲染管线，可选运行时启用）
// 运行时启用：设置环境变量 HAPPLAYER_USE_VULKAN=1，OpenGL 默认路径保持不变。
#include "vulkan_renderer.h"
// Vulkan 阶段 E：环境变量开关（文件作用域静态；upload/closeLayer/draw 等静态函数也要访问）
// 设置 HAPPLAYER_USE_VULKAN=1 走 Vulkan 路径；未设置时保持 OpenGL 默认路径完全不变
static const bool g_useVulkan = std::getenv("HAPPLAYER_USE_VULKAN") != nullptr;
#else
static const bool g_useVulkan = false;
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT  0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_RED_RGTC1          0x8DBB  // BC4，HAP Q Alpha 的 alpha 平面

typedef void (APIENTRY* PFNGLCOMPRESSEDTEXIMAGE2DPROC)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
typedef void (APIENTRY* PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*);
static PFNGLCOMPRESSEDTEXIMAGE2DPROC    pglCompressedTexImage2D    = nullptr;
static PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC pglCompressedTexSubImage2D = nullptr;

// P0.1 PBO 异步上传所需常量（GL.h 是 OpenGL 1.1，没有这些）
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER         0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW                 0x88E0
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT               0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT   0x0008
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE  0x9117
#endif
#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED             0xFFFFFFFFFFFFFFFFull
#endif
// GLsync 在 MSVC gl.h 里可能缺失，typedef 一个不透明结构指针
typedef struct __GLsync* GLsync;
#ifndef GLuint64
typedef unsigned __int64 GLuint64;
#endif
// OpenGL 2.0 shader 常量
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_DELETE_STATUS
#define GL_DELETE_STATUS 0x8B80
#endif

// PBO + Fence 函数指针（OpenGL 1.5+ / 3.2+，需要动态加载）
typedef void   (APIENTRY* PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY* PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void   (APIENTRY* PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void   (APIENTRY* PFNGLBUFFERDATAPROC)(GLenum, GLsizei, const void*, GLenum);
typedef void*  (APIENTRY* PFNGLMAPBUFFERRANGEPROC)(GLenum, GLsizei, GLsizei, GLbitfield);
typedef GLboolean (APIENTRY* PFNGLUNMAPBUFFERPROC)(GLenum);
typedef GLsync (APIENTRY* PFNGLFENCESYNCPROC)(GLenum, GLbitfield);
typedef void   (APIENTRY* PFNGLDELETESYNCPROC)(GLsync);
typedef GLenum (APIENTRY* PFNGLCLIENTWAITSYNCPROC)(GLsync, GLbitfield, GLuint64);
static PFNGLGENBUFFERSPROC       pglGenBuffers       = nullptr;
static PFNGLDELETEBUFFERSPROC    pglDeleteBuffers    = nullptr;
static PFNGLBINDBUFFERPROC       pglBindBuffer       = nullptr;
static PFNGLBUFFERDATAPROC       pglBufferData       = nullptr;
static PFNGLMAPBUFFERRANGEPROC   pglMapBufferRange   = nullptr;
static PFNGLUNMAPBUFFERPROC      pglUnmapBuffer      = nullptr;
static PFNGLFENCESYNCPROC        pglFenceSync        = nullptr;
static PFNGLDELETESYNCPROC       pglDeleteSync       = nullptr;
static PFNGLCLIENTWAITSYNCPROC   pglClientWaitSync   = nullptr;

// P1.1 HAP Q shader 所需函数（OpenGL 2.0+，手动 typedef + 动态加载）
typedef GLuint (APIENTRY* PFNGLCREATESHADERPROC)(GLenum);
typedef void   (APIENTRY* PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void   (APIENTRY* PFNGLCOMPILESHADERPROC)(GLuint);
typedef void   (APIENTRY* PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, char*);
typedef void   (APIENTRY* PFNGLDELETESHADERPROC)(GLuint);
typedef GLuint (APIENTRY* PFNGLCREATEPROGRAMPROC)(void);
typedef void   (APIENTRY* PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void   (APIENTRY* PFNGLLINKPROGRAMPROC)(GLuint);
typedef void   (APIENTRY* PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, char*);
typedef void   (APIENTRY* PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void   (APIENTRY* PFNGLUSEPROGRAMPROC)(GLuint);
typedef GLint  (APIENTRY* PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const char*);
typedef void   (APIENTRY* PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void   (APIENTRY* PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef GLint  (APIENTRY* PFNGLGETATTRIBLOCATIONPROC)(GLuint, const char*);
typedef void   (APIENTRY* PFNGLVERTEXATTRIB2FPROC)(GLuint, GLfloat, GLfloat);
typedef void   (APIENTRY* PFNGLBINDATTRIBLOCATIONPROC)(GLuint, GLuint, const char*);
typedef void   (APIENTRY* PFNGLACTIVETEXTUREPROC)(GLenum);
static PFNGLCREATESHADERPROC       pglCreateShader         = nullptr;
static PFNGLSHADERSOURCEPROC       pglShaderSource         = nullptr;
static PFNGLCOMPILESHADERPROC      pglCompileShader        = nullptr;
static PFNGLGETSHADERIVPROC        pglGetShaderiv          = nullptr;
static PFNGLGETSHADERINFOLOGPROC   pglGetShaderInfoLog     = nullptr;
static PFNGLDELETESHADERPROC       pglDeleteShader         = nullptr;
static PFNGLCREATEPROGRAMPROC      pglCreateProgram        = nullptr;
static PFNGLATTACHSHADERPROC       pglAttachShader         = nullptr;
static PFNGLLINKPROGRAMPROC        pglLinkProgram          = nullptr;
static PFNGLGETPROGRAMIVPROC       pglGetProgramiv         = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC  pglGetProgramInfoLog    = nullptr;
static PFNGLDELETEPROGRAMPROC      pglDeleteProgram        = nullptr;
static PFNGLUSEPROGRAMPROC         pglUseProgram           = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC pglGetUniformLocation   = nullptr;
static PFNGLUNIFORM1IPROC          pglUniform1i            = nullptr;
static PFNGLUNIFORM1FPROC          pglUniform1f            = nullptr;
static PFNGLGETATTRIBLOCATIONPROC  pglGetAttribLocation     = nullptr;
static PFNGLVERTEXATTRIB2FPROC     pglVertexAttrib2f       = nullptr;
static PFNGLBINDATTRIBLOCATIONPROC pglBindAttribLocation   = nullptr;
static PFNGLACTIVETEXTUREPROC      pglActiveTexture        = nullptr;

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#endif

// PBO/Fence 全套函数加载成功才为 true，否则 upload 走同步路径（B-1 修复）
static bool g_pboAvailable = false;

// ---- HAP Q 着色器：YCoCg DXT5 → RGB（公式按 Vidvox 官方参考实现）----
// DXT5 纹理通道含义：R=Co，G=Cg，B=scale 因子，A=Y（亮度存在 alpha 通道，精度更高）
//   scale = B * (255/8) + 1
//   Co = (R - 128/255) / scale
//   Cg = (G - 128/255) / scale
//   RGB = (Y + Co - Cg,  Y + Cg,  Y - Co - Cg)，alpha 恒为 1（Hap Q 无透明）
static const char* kHapQVertSrc =
    "#version 120\n"
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main(){\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

static const char* kHapQFragSrc =
    "#version 120\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uAlpha;\n"
    "void main(){\n"
    "    vec4 yc = texture2D(uTex, vUV);\n"
    "    float scale = yc.b * (255.0 / 8.0) + 1.0;\n"
    "    float Co = (yc.r - 0.5019608) / scale;\n"
    "    float Cg = (yc.g - 0.5019608) / scale;\n"
    "    float Y  = yc.a;\n"
    "    gl_FragColor = vec4(Y + Co - Cg, Y + Cg, Y - Co - Cg, uAlpha);\n"
    "}\n";

static GLuint g_hapQProgram = 0;
static GLint  g_hapQAttrPos = -1;
static GLint  g_hapQAttrUV  = -1;
static GLint  g_hapQUTex    = -1;
static GLint  g_hapQUAlpha  = -1;

// ---- HAP Q Alpha（0x0D 多图帧：YCoCg DXT5 颜色 + BC4 alpha 平面）----
static const char* kHapQAlphaFragSrc =
    "#version 120\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"    // YCoCg DXT5
    "uniform sampler2D uTexA;\n"   // BC4 alpha（.r）
    "uniform float uAlpha;\n"
    "void main(){\n"
    "    vec4 yc = texture2D(uTex, vUV);\n"
    "    float scale = yc.b * (255.0 / 8.0) + 1.0;\n"
    "    float Co = (yc.r - 0.5019608) / scale;\n"
    "    float Cg = (yc.g - 0.5019608) / scale;\n"
    "    float Y  = yc.a;\n"
    "    float a  = texture2D(uTexA, vUV).r;\n"
    "    gl_FragColor = vec4(Y + Co - Cg, Y + Cg, Y - Co - Cg, a * uAlpha);\n"
    "}\n";

static GLuint g_hapQAProgram = 0;
static GLint  g_hapQAUTex   = -1;
static GLint  g_hapQAUTexA  = -1;
static GLint  g_hapQAUAlpha = -1;

// 编译 + 链接一个 program（vs 固定用 kHapQVertSrc，attribute 绑 0/1）
static GLuint compileShader(GLenum type, const char* src, std::string& err);  // 前向声明
static GLuint buildProgram(const char* fragSrc, const char* tag) {
    std::string err;
    GLuint vs = compileShader(GL_VERTEX_SHADER, kHapQVertSrc, err);
    if (!vs) { std::cerr << "[" << tag << "] 顶点编译失败: " << err << "\n"; return 0; }
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc, err);
    if (!fs) {
        std::cerr << "[" << tag << "] 片段编译失败: " << err << "\n";
        pglDeleteShader(vs);
        return 0;
    }
    GLuint prog = pglCreateProgram();
    pglAttachShader(prog, vs);
    pglAttachShader(prog, fs);
    if (pglBindAttribLocation) {
        pglBindAttribLocation(prog, 0, "aPos");
        pglBindAttribLocation(prog, 1, "aUV");
    }
    pglLinkProgram(prog);
    GLint ok = 0;
    pglGetProgramiv(prog, GL_LINK_STATUS, &ok);
    pglDeleteShader(vs);
    pglDeleteShader(fs);
    if (!ok) {
        char buf[1024];
        pglGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        std::cerr << "[" << tag << "] 链接失败: " << buf << "\n";
        pglDeleteProgram(prog);
        return 0;
    }
    std::cout << "[" << tag << "] 已编译 (program=" << prog << ")\n";
    return prog;
}

static void initShaders() {
    if (!pglCreateShader) { std::cerr << "[shader] 驱动不支持 GLSL\n"; return; }
    g_hapQProgram = buildProgram(kHapQFragSrc, "HAP-Q shader");
    if (g_hapQProgram) {
        g_hapQAttrPos = pglGetAttribLocation(g_hapQProgram, "aPos");
        g_hapQAttrUV  = pglGetAttribLocation(g_hapQProgram, "aUV");
        g_hapQUTex    = pglGetUniformLocation(g_hapQProgram, "uTex");
        g_hapQUAlpha  = pglGetUniformLocation(g_hapQProgram, "uAlpha");
    }
    g_hapQAProgram = buildProgram(kHapQAlphaFragSrc, "HAP-QA shader");
    if (g_hapQAProgram) {
        // attribute 布局与 g_hapQProgram 相同（0=aPos, 1=aUV），共用 g_hapQAttr*
        g_hapQAUTex   = pglGetUniformLocation(g_hapQAProgram, "uTex");
        g_hapQAUTexA  = pglGetUniformLocation(g_hapQAProgram, "uTexA");
        g_hapQAUAlpha = pglGetUniformLocation(g_hapQAProgram, "uAlpha");
    }
}

static GLuint compileShader(GLenum type, const char* src, std::string& err) {
    GLuint sh = pglCreateShader(type);
    pglShaderSource(sh, 1, &src, nullptr);
    pglCompileShader(sh);
    GLint ok = 0;
    pglGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        pglGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
        err = buf;
        pglDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool initHapQShader() {   // 兼容包装：实际走 initShaders
    initShaders();
    return g_hapQProgram != 0;
}

static const size_t RING_CAP = 6;   // 每层预解码帧数（6 帧≈200ms 缓冲；4K 时 12 帧=每层 100MB，浪费且无用）

// ---------------- 图层 ----------------
// 多 codec 支持：HAP（DXT 压缩纹理）/ Animation QTRLE（RGB）/ H.264（YUV420）
//  - DXT 数据直接走 GPU 压缩纹理上传（P0.1 PBO 路径）
//  - RGBA 数据走普通 glTexImage2D / glTexSubImage2D（无 PBO，CPU 同步）
enum class CodecType { HAP, ANIMATION_QTRLE, H264 };
enum class PixelFormat { DXT, RGBA };   // 帧上传到 GPU 时的格式

struct Frame {                      // 解码线程产出的一帧纹理数据
    uint64_t gen = 0;               // 第几轮（seek/回卷时 +1，过期帧直接丢弃）
    int64_t  index = 0;
    PixelFormat fmt = PixelFormat::DXT;
    hap::PixFmt dxtFmt = hap::PixFmt::DXT5;   // 仅 DXT 时有效
    std::vector<uint8_t> buf;       // DXT 压缩数据 / RGBA8 像素
    size_t bufSize = 0;             // 实际数据字节数
    // 预加载层专用：直接指向 L.preloadedFrames 里的帧，整帧零拷贝（B-16 修复）
    // 非空时上传用 sharedBuf->data()，buf 为空
    const std::vector<uint8_t>* sharedBuf = nullptr;
    // HAP Q Alpha 专用：第二平面（BC4 压缩 alpha），无第二平面时为空
    std::vector<uint8_t> buf2;
    size_t buf2Size = 0;
};

struct DecState {                   // 解码线程状态（堆上持有，Layer 才能移动）
    std::thread th;
    std::shared_mutex mtx;          // P0.3 v2: 读写锁分离（decoderRun unique_lock，updateLayer shared_lock）
    std::condition_variable_any cv;     // cv.wait 需配 shared_mutex → 必须用 condition_variable_any
    std::deque<Frame> ring;
    std::vector<std::vector<uint8_t>> freeBufs; // buffer 回收复用
    uint64_t gen = 0;
    uint64_t framesDecoded = 0;
    bool active = false;            // 属于当前队列才解码
    bool pendingReset = false;      // 主线程请求回卷到 0
    bool exiting = false;
    bool decoderEof = false;        // 不循环层已到文件尾
    // 主线程每帧写入的播放位置（帧序号）。解码线程只允许领先它 RING_CAP 帧——
    // 没有这道闸，worker 会以最大速度把整文件反复解码（ring 全是"未来帧"，画面冻结/乱跳）
    std::atomic<int64_t> presentIndex{0};
    std::atomic<int64_t> nextIndexDbg{0};   // 解码线程已读到的帧序号（排障观测用）

    // P0.2 snappy 并行解压：主解码线程只读 packet 并 push 到 pktQueue，
    // snappy worker 线程从 pktQueue 取 packet 做 hap::decodeInto，再把结果 push 到 ring。
    // 锁 mtx 仍保护 ring/cv/exiting 等共享状态；pktMtx/pktCv 独立保护 pktQueue。
    // B-10 修复：序号与 gen 在【提交时】绑定，worker 完成顺序不再影响帧序
    struct PktJob {
        AVPacket* pkt;
        int64_t   index;   // 提交时分配的帧序号（= 流内顺序）
        uint64_t  gen;     // 提交时的轮次（reset/回卷后的残留 job 会被主线程按 gen 丢弃）
    };
    std::deque<PktJob> pktQueue;         // 待解压 packet（HAP only）
    std::mutex pktMtx;
    std::condition_variable pktCv;
    std::vector<std::thread> workers;      // snappy worker 线程
    bool workersExit = false;
    std::atomic<bool> workersStarted{false};
};

struct Layer {
    std::string path;
    std::string name;
    double start = 0.0;   // 队列时钟走到该秒数才入场
    bool   loop  = true;  // 循环层不决定队列结束，队列结束时被切掉

    // 可选自定义布局（图层行第 4~7 字段 |x%|y%|宽%|音量%）
    bool  customRect = false; // false = contain 适配居中（默认）
    float rx = 0, ry = 0;     // 矩形左上角占窗口宽/高的百分比
    float rw = 100;           // 矩形宽占窗口宽的百分比（高按比例）
    float volume = 1.0f;      // 该层内嵌音频音量 0~1（多源混音用）

    AVFormatContext* fmt = nullptr; // 仅解码线程访问
    int streamIdx = -1;
    // P1.4：HAP 文件可能自带音频流；>=0 时优先用其音频（替代独立 wav）
    int audioStreamIdx = -1;
    double fps = 30.0;
    double duration = 0.0;
    int w = 0, h = 0;

    // 资源预加载：< preloadMaxDuration 秒的非 HAP 层一次性解到内存
    // 避免运行时冷启动解码延迟。HAP 不需要，DXT 数据从 packet 直接取即可。
    std::vector<std::vector<uint8_t>> preloadedFrames; // 每帧 RGBA8 数据
    PixelFormat preloadedFmt = PixelFormat::RGBA;       // 预加载层必为 RGBA
    bool preloaded = false;                             // true 时 decoderRun 直读这里

    // 多 codec 支持：HAP / ANIMATION_QTRLE / H264
    // HAP 走 hap::decodeInto（DXT 压缩纹理）；其他走 FFmpeg 软解 → sws_scale → RGBA8
    CodecType codecType = CodecType::HAP;

    // FFmpeg 软解状态（仅 QTRLE / H264 时用，HAP 为 nullptr）
    AVCodecContext* codecCtx = nullptr;
    AVFrame*        swFrame  = nullptr;    // 软解出的原始帧（YUV）
    AVFrame*        rgbFrame = nullptr;    // sws_scale 目标（RGBA8）
    SwsContext*     swsCtx   = nullptr;
    uint8_t*        rgbBuf   = nullptr;    // rgbFrame->data[0] 指向的外部缓冲
    int             rgbBufSize = 0;

    // P2.3 NVDEC 状态（仅 H.264 + 有 NVIDIA 卡时用）
    // hw_device_ctx 持有 CUDA context；codecCtx->hw_device_ctx 是它的引用
    // useNVDEC 标记是否启用硬解（debug 日志 + decoderRun 分支用）
    AVBufferRef*    hw_device_ctx = nullptr;   // CUDA device context（每层独立 ref）
    bool            useNVDEC = false;          // true 时 decoderRun 走 NVDEC 路径
    // AV_PIX_FMT_CUDA → CPU NV12 中转帧（sws_scale 不认识 CUDA 帧，需要先 transfer）
    AVFrame*        hwFrame  = nullptr;        // receive_frame 拿到的 CUDA 帧
    AVFrame*        nv12Frame = nullptr;       // transfer 后的 NV12 CPU 帧（喂给 sws_scale）

    // 主线程（渲染）状态
    GLuint tex = 0;
    GLuint texA = 0;        // HAP Q Alpha 的 alpha 平面纹理（BC4）
    bool texAllocated = false;
    int64_t shownIndex = -1;
    double prevLt = -1.0;
    bool visible = false;
    std::string state = "waiting";

    // P0.1 PBO 异步上传：双缓冲 ping-pong + fence sync
    // DXT 和 RGBA 共用同一组 PBO + fence（同一层不会变格式，容量在首次分配时一次性设好）
    GLuint pbo[2] = {0, 0};
    GLsync pboFence[2] = {nullptr, nullptr};
    int pboIdx = 0;
    size_t pboCap = 0;   // 当前 PBO 容量（用于 fence 等够大）
    // 纹理格式标志：false=DXT（compressedTexSubImage2D），true=RGBA8（TexSubImage2D）
    // 首次分配时设置；同层不会改
    bool texIsRGBA = false;

    // P1.1 HAP Q 标志：true 时 GPU 上传仍是 DXT5，但绘制走 YCoCg→RGB shader
    bool isHapQ = false;

#ifdef HAPPLAYER_HAS_VULKAN
    // Vulkan 阶段 E：每层一张 DXT/RGBA 纹理（持久 staging + 逐帧 memcpy）
    vk_hapq::DXTImage vkTex;
    bool              vkDirty = false;
#endif

    std::unique_ptr<DecState> dec;
};

struct Queue {
    std::vector<Layer> layers;
};

// 窗口/显示配置（[window] 段，命令行可覆盖）
struct WindowCfg {
    int w = 1280, h = 720;   // 窗口分辨率（跨屏就写总宽，如 3840x1080）
    int x = 0, y = 0;        // 相对目标显示器左上角的偏移
    int monitor = 1;         // 从第几个显示器开始（1 起）
    int scale = 100;         // 内容缩放百分比（100 = 原始适配大小，以窗口中心缩放）
    double fade = 0.0;       // 场间淡入淡出时长（秒，0 = 硬切）
    bool fullscreen = false; // 启动即全屏（占满目标显示器）
    bool borderless = false; // 无边框
};

// P2.3 NVDEC 探测：双保险
//   1) 检查 NVIDIA 驱动 DLL（nvcuda.dll / nvapi64.dll）— 任意一个存在即可
//   2) 用 FFmpeg 的 av_hwdevice_iterate_types 看 AV_HWDEVICE_TYPE_CUDA 是否编译进来
// 两关都过 → 走 NVDEC；任一失败 → 走软解。绝不依赖 dxgi 枚举（避免运行时开销）。
// 全局只探测一次（首次调用后缓存结果），后续 openLayer 直接看缓存。
static bool g_nvdecAvailable = false;
static bool g_nvdecProbed = false;
static bool g_forceDisableNvdec = false;  // 测试用：环境变量 HAPPLAYER_NO_NVDEC=1 时关闭

static bool probeNvidiaDLL() {
    // 用 GetSystemDirectoryW + GetFileAttributesW 探测 nvcuda.dll / nvapi64.dll 是否存在
    //   - 不加载 DLL（避免真初始化驱动，毫秒级完成）
    //   - 系统目录就是 System32（X64 系统），不必跨平台
    WCHAR sysDir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    // 拼接路径
    WCHAR path[MAX_PATH] = {};
    // 用 lstrcpyW + lstrcatW 避免 C 字符串越界
    lstrcpynW(path, sysDir, MAX_PATH);
    lstrcatW(path, L"\\nvcuda.dll");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;

    lstrcpynW(path, sysDir, MAX_PATH);
    lstrcatW(path, L"\\nvapi64.dll");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;

    return false;
}

static bool hasNvidiaGPU() {
    if (g_nvdecProbed) return g_nvdecAvailable;
    g_nvdecProbed = true;
    g_nvdecAvailable = false;

    // 测试开关：HAPPLAYER_NO_NVDEC=1 强制走软解（验证 fallback 路径）
    const char* envForce = std::getenv("HAPPLAYER_NO_NVDEC");
    if (envForce && envForce[0] == '1') {
        g_forceDisableNvdec = true;
        std::cout << "[NVDEC] 环境变量 HAPPLAYER_NO_NVDEC=1 强制软解\n";
        return false;
    }

    // 关 1：驱动 DLL 在不在
    if (!probeNvidiaDLL()) {
        std::cout << "[NVDEC] 未检测到 NVIDIA 驱动 DLL，走软解\n";
        return false;
    }

    // 关 2：FFmpeg 编译时是否带 AV_HWDEVICE_TYPE_CUDA
    AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
    bool cudaCompiled = false;
    while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
        if (t == AV_HWDEVICE_TYPE_CUDA) { cudaCompiled = true; break; }
    }
    if (!cudaCompiled) {
        std::cout << "[NVDEC] FFmpeg 未编译 CUDA hwaccel，走软解\n";
        return false;
    }

    // 关 3（可选）：实际创 CUDA context — 验证驱动能起来
    // 不在这一步做（开销大）；让 openLayer 真用时再失败 + fallback。
    g_nvdecAvailable = true;
    std::cout << "[NVDEC] 检测到 NVIDIA GPU + CUDA hwaccel，启用硬解\n";
    return true;
}

static bool openLayer(Layer& L) {
    if (avformat_open_input(&L.fmt, L.path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[错误] 打不开: " << L.path << "\n";
        return false;
    }
    if (avformat_find_stream_info(L.fmt, nullptr) < 0) {
        std::cerr << "[错误] 读取流信息失败: " << L.path << "\n";
        return false;
    }
    L.streamIdx = av_find_best_stream(L.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (L.streamIdx < 0) {
        std::cerr << "[错误] 没有视频流: " << L.path << "\n";
        return false;
    }
    AVStream* st = L.fmt->streams[L.streamIdx];
    // 多 codec 分支：根据 codec_id 决定走 HAP（DXT 压缩纹理）还是 FFmpeg 软解
    switch (st->codecpar->codec_id) {
        case AV_CODEC_ID_HAP:    L.codecType = CodecType::HAP;            break;
        case AV_CODEC_ID_QTRLE:  L.codecType = CodecType::ANIMATION_QTRLE; break;
        case AV_CODEC_ID_H264:   L.codecType = CodecType::H264;            break;
        default:
            std::cerr << "[错误] 不支持的编码: " << L.path
                      << " (codec_id=" << (int)st->codecpar->codec_id << ")\n";
            return false;
    }
    L.w = st->codecpar->width;
    L.h = st->codecpar->height;

    AVRational fr = av_guess_frame_rate(L.fmt, st, nullptr);
    double fps = av_q2d(fr);
    if (std::isfinite(fps) && fps > 0.1) L.fps = fps;

    if (st->duration != AV_NOPTS_VALUE)
        L.duration = st->duration * av_q2d(st->time_base);
    else if (L.fmt->duration != AV_NOPTS_VALUE)
        L.duration = L.fmt->duration / (double)AV_TIME_BASE;
    else if (st->nb_frames > 0)
        L.duration = st->nb_frames / L.fps;

    size_t pos = L.path.find_last_of("/\\");
    L.name = (pos == std::string::npos) ? L.path : L.path.substr(pos + 1);

    // P1.4：HAP 文件可能自带音频流；找到就记录（>0 是音频流）
    L.audioStreamIdx = av_find_best_stream(L.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // 非 HAP 路径：用 FFmpeg 软解，需要初始化 codecCtx / swFrame / rgbFrame / swsCtx
    // P2.3: H.264 且机器有 NVIDIA 卡时优先走 h264_cuvid（硬解），失败回退软解
    if (L.codecType != CodecType::HAP) {
        const AVCodec* codec = nullptr;
        bool tryNvdec = (L.codecType == CodecType::H264) && hasNvidiaGPU();
        bool cuvidChosen = false;

        if (tryNvdec) {
            codec = avcodec_find_decoder_by_name("h264_cuvid");
            if (codec) {
                cuvidChosen = true;
            } else {
                std::cerr << "[NVDEC] 找不到 h264_cuvid 解码器（FFmpeg 编译缺失），回退软解\n";
                codec = nullptr;
            }
        }
        if (!codec) {
            codec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!codec) {
                std::cerr << "[错误] 找不到解码器: " << L.name << "\n";
                return false;
            }
        }
        L.codecCtx = avcodec_alloc_context3(codec);
        if (!L.codecCtx) {
            std::cerr << "[错误] 分配 codecCtx 失败: " << L.name << "\n";
            return false;
        }
        if (avcodec_parameters_to_context(L.codecCtx, st->codecpar) < 0) {
            std::cerr << "[错误] 复制 codecpar 失败: " << L.name << "\n";
            return false;
        }
        L.codecCtx->thread_count = 1;  // 每层一个软解线程够了；CPU 多核留给层间并行

        // P2.3 NVDEC：把 hw_device_ctx 绑到 codecCtx
        // 失败（驱动问题 / GPU 太被占） → 回退软解，不让单层卡死整个播放器
        if (cuvidChosen) {
            int hwrc = av_hwdevice_ctx_create(&L.hw_device_ctx, AV_HWDEVICE_TYPE_CUDA,
                                              nullptr, nullptr, 0);
            if (hwrc < 0) {
                char errbuf[128]; av_strerror(hwrc, errbuf, sizeof(errbuf));
                std::cerr << "[NVDEC] 创建 CUDA device 失败 (" << errbuf << ")，回退软解\n";
                if (L.hw_device_ctx) { av_buffer_unref(&L.hw_device_ctx); L.hw_device_ctx = nullptr; }
                avcodec_free_context(&L.codecCtx);
                L.codecCtx = nullptr;
                codec = avcodec_find_decoder(st->codecpar->codec_id);
                if (!codec) { std::cerr << "[错误] 找不到解码器: " << L.name << "\n"; return false; }
                L.codecCtx = avcodec_alloc_context3(codec);
                if (!L.codecCtx) { std::cerr << "[错误] 分配 codecCtx 失败: " << L.name << "\n"; return false; }
                if (avcodec_parameters_to_context(L.codecCtx, st->codecpar) < 0) {
                    std::cerr << "[错误] 复制 codecpar 失败: " << L.name << "\n"; return false;
                }
                L.codecCtx->thread_count = 1;
            } else {
                L.codecCtx->hw_device_ctx = av_buffer_ref(L.hw_device_ctx);
                L.useNVDEC = true;
                std::cout << "    [NVDEC] h264_cuvid 已启动（CUDA context 创建成功）\n";
            }
        }

        if (avcodec_open2(L.codecCtx, codec, nullptr) < 0) {
            std::cerr << "[错误] 打开解码器失败: " << L.name << "\n";
            return false;
        }
        L.swFrame  = av_frame_alloc();
        L.rgbFrame = av_frame_alloc();
        if (!L.swFrame || !L.rgbFrame) {
            std::cerr << "[错误] 分配 AVFrame 失败: " << L.name << "\n";
            return false;
        }
        // 预分配 RGBA 缓冲：w*h*4，sws_scale 直接写到这里
        L.rgbBufSize = L.w * L.h * 4;
        L.rgbBuf = (uint8_t*)av_malloc(L.rgbBufSize);
        if (!L.rgbBuf) {
            std::cerr << "[错误] 分配 rgbBuf 失败: " << L.name << "\n";
            return false;
        }
        // P2.3 NVDEC 路径需要额外两个 AVFrame：
        //   hwFrame   — 接收 AV_PIX_FMT_CUDA 帧
        //   nv12Frame — transfer 后的 CPU NV12 帧（喂 sws_scale）
        if (L.useNVDEC) {
            L.hwFrame   = av_frame_alloc();
            L.nv12Frame = av_frame_alloc();
            if (!L.hwFrame || !L.nv12Frame) {
                std::cerr << "[错误] 分配 NVDEC AVFrame 失败: " << L.name << "\n";
                return false;
            }
        }
        // sws_ctx：源 = NVDEC 时是 NV12；软解时是 codecCtx->pix_fmt（通常 YUV420P）
        // P2.3 NVDEC 帧经 nv12Frame 转回 CPU 后是 AV_PIX_FMT_NV12
        AVPixelFormat srcFmt = L.useNVDEC ? AV_PIX_FMT_NV12 : L.codecCtx->pix_fmt;
        L.swsCtx = sws_getContext(
            L.w, L.h, srcFmt,
            L.w, L.h, AV_PIX_FMT_RGBA,
            SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!L.swsCtx) {
            std::cerr << "[错误] sws_getContext 失败: " << L.name << "\n";
            return false;
        }
        av_image_fill_arrays(L.rgbFrame->data, L.rgbFrame->linesize,
                             L.rgbBuf, AV_PIX_FMT_RGBA, L.w, L.h, 1);
    }

    const char* codecName = (L.codecType == CodecType::HAP) ? "HAP"
                          : (L.codecType == CodecType::ANIMATION_QTRLE) ? "QTRLE"
                          : "H.264";
    std::cout << "    " << L.name << "  " << L.w << "x" << L.h
              << "  " << L.fps << "fps  " << L.duration << "s"
              << "  start=" << L.start << "s  loop=" << (L.loop ? "是" : "否")
              << "  [" << codecName << "]"
              << (L.audioStreamIdx >= 0 ? "  [自带音频]" : "") << "\n";
    return true;
}

// ---------------- 解码线程 ----------------
// 调度：mutex + cv.wait，frame push 流程：
//   1) wait_for（直到「应该解码」= active && !eof && ring.size<RING_CAP）
//   2) 取一个空闲 buffer（从 freeBufs 池里拿一个 std::vector）
//   3) 解码 → memcpy/assign → push Frame（自带 std::vector）到 ring
//   4) 通知 cv 让主线程被唤醒
static void decoderRun(Layer& L) {
    // 解码线程让路：主线程（渲染/上传）优先抢 CPU，防止高负载时渲染线程被饿死
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DecState& d = *L.dec;
    int64_t nextIndex = 0;
    auto syncNi = [&] { d.nextIndexDbg.store(nextIndex, std::memory_order_relaxed); };
    AVPacket* pkt = av_packet_alloc();
    for (;;) {
        {
            std::unique_lock<std::shared_mutex> lk(d.mtx);
            d.cv.wait(lk, [&] {
                return d.exiting || d.pendingReset ||
                       (d.active && !d.decoderEof && d.ring.size() < RING_CAP &&
                        nextIndex <= d.presentIndex.load(std::memory_order_relaxed) + (int64_t)RING_CAP);
            });
            if (d.exiting) break;
            if (d.pendingReset) {
                if (L.preloaded) {
                    // 预加载层：把 ring 清空、nextIndex 归 0，无需 seek/flush
                    for (auto& fr : d.ring) d.freeBufs.push_back(std::move(fr.buf));
                    d.ring.clear();
                    nextIndex = 0;
                    d.gen++;
                    d.decoderEof = false;
                    d.pendingReset = false;
                } else {
                    av_seek_frame(L.fmt, L.streamIdx, 0, AVSEEK_FLAG_BACKWARD);
                    avformat_flush(L.fmt);
                    if (L.codecCtx) avcodec_flush_buffers(L.codecCtx);
                    nextIndex = 0;
                    d.gen++;
                    d.decoderEof = false;
                    for (auto& fr : d.ring) d.freeBufs.push_back(std::move(fr.buf));
                    d.ring.clear();
                    d.pendingReset = false;
                }
                // B-11 修复：reset/回卷时排空待解压队列，残留 job 带旧 gen 也没处去
                {
                    std::lock_guard<std::mutex> plk(d.pktMtx);
                    for (auto& job : d.pktQueue) av_packet_free(&job.pkt);
                    d.pktQueue.clear();
                }
                continue;
            }
        }

        // 预加载分支：从 L.preloadedFrames 直接拷贝到 ring，绕开 packet 读取
        if (L.preloaded && !L.preloadedFrames.empty()) {
            int64_t total = (int64_t)L.preloadedFrames.size();
            int64_t idx = nextIndex % total;
            // 锁内 push（保证帧索引与 nextIndex 一致）
            std::unique_lock<std::shared_mutex> lk(d.mtx);
            // 容量保护：ring 已满就不喂
            if (d.ring.size() >= RING_CAP) continue;
            Frame fr;
            fr.gen = d.gen;
            fr.index = nextIndex++;
            fr.fmt = L.preloadedFmt;
            fr.dxtFmt = hap::PixFmt::DXT5; // RGBA 路径不用
            fr.sharedBuf = &L.preloadedFrames[(size_t)idx]; // B-16：共享指针，零拷贝
            fr.bufSize = fr.sharedBuf->size();
            d.ring.push_back(std::move(fr));
            d.framesDecoded++;
            // 单遍播完：到 total 时标记 EOF，等 resetLayer 重置（B-13 修复：循环层不标记，继续取模循环）
            if (!L.loop && nextIndex >= total) d.decoderEof = true;
            continue;
        }
        // 锁外：取一个空闲 buffer
        std::vector<uint8_t> buf;
        {
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            if (!d.freeBufs.empty()) {
                buf = std::move(d.freeBufs.back());
                d.freeBufs.pop_back();
            }
        }

        // 锁外：读 packet
        int ret;
        for (;;) {
            ret = av_read_frame(L.fmt, pkt);
            if (ret < 0) break;
            if (pkt->stream_index != L.streamIdx) { av_packet_unref(pkt); continue; }
            break;
        }
        syncNi();
        if (ret < 0) {
            // 文件尾：挂起等待。循环层的回卷由主线程按时钟回卷点通过 pendingReset 触发
            // （解码线程自行 seek 会抢跑：预读让它提前 ~RING_CAP 帧到尾，抢跑后
            //   presentIndex 闸门还停在上一轮末尾 → 反复绕整文件 → 回卷卡几秒）
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            d.decoderEof = true;
            d.freeBufs.push_back(std::move(buf));
            continue;
        }

        size_t dataSize = 0;
        hap::PixFmt dxtFmt = hap::PixFmt::DXT5;
        PixelFormat fmt = PixelFormat::DXT;
        bool decodeOk = false;

        if (L.codecType == CodecType::HAP) {
            // P0.2 snappy 并行解压：主线程只 push packet 到 pktQueue，
            // 由 snappy worker 线程做 hap::decodeInto 并 push 到 ring。
            // 此时不释放 packet（worker 解压完成后负责 av_packet_free）。
            // 当前 buf 归还 freeBufs（worker 会自己从 freeBufs 取新 buf）。
            // B-10 修复：帧序号与 gen 在提交时绑定（worker 完成顺序不再影响帧序）。
            uint64_t genSnap;
            int64_t idx;
            {
                std::lock_guard<std::shared_mutex> lk(d.mtx);
                d.freeBufs.push_back(std::move(buf));
                genSnap = d.gen;
                idx = nextIndex++;
            }
            {
                std::lock_guard<std::mutex> lk(d.pktMtx);
                d.pktQueue.push_back({pkt, idx, genSnap});
            }
            d.pktCv.notify_one();
            // pkt 已被 worker 接管，主线程立即重用一个新 AVPacket 给下一轮
            pkt = av_packet_alloc();
            dataSize = 0;
            decodeOk = true;  // 标记"已接管"，跳过下方 ring.push（worker 会做）
        } else {
            // QTRLE / H.264 路径：avcodec_send_packet + receive_frame + sws_scale
            // P2.3 NVDEC：receive_frame 拿到的是 AV_PIX_FMT_CUDA（GPU memory），
            //              必须先 av_hwframe_transfer_data 拷回 CPU NV12，再走 sws_scale
            int sendRet = avcodec_send_packet(L.codecCtx, pkt);
            av_packet_unref(pkt);
            if (sendRet == 0) {
                ret = avcodec_receive_frame(L.codecCtx, L.swFrame);
                if (ret == 0) {
                    AVFrame* srcFrame = L.swFrame;          // 默认软解：直接喂 sws_scale
                    if (L.useNVDEC && L.swFrame->format == AV_PIX_FMT_CUDA) {
                        // NVDEC 路径：transfer CUDA → CPU NV12
                        av_frame_unref(L.nv12Frame);
                        int trc = av_hwframe_transfer_data(L.nv12Frame, L.swFrame, 0);
                        if (trc < 0) {
                            char errbuf[128]; av_strerror(trc, errbuf, sizeof(errbuf));
                            std::cerr << "[NVDEC] transfer_data 失败: " << errbuf << "\n";
                            av_frame_unref(L.swFrame);
                            std::lock_guard<std::shared_mutex> lk(d.mtx);
                            d.freeBufs.push_back(std::move(buf));
                            continue;
                        }
                        srcFrame = L.nv12Frame;
                    } else if (L.useNVDEC) {
                        // useNVDEC=true 但 frame format 不是 CUDA（罕见，比如首帧）
                        // 把它当软解路径处理（srcFrame 仍是 swFrame）
                    }
                    sws_scale(L.swsCtx,
                              (const uint8_t* const*)srcFrame->data, srcFrame->linesize,
                              0, L.h, L.rgbFrame->data, L.rgbFrame->linesize);
                    buf.assign(L.rgbBuf, L.rgbBuf + L.rgbBufSize);
                    dataSize = L.rgbBufSize;
                    fmt = PixelFormat::RGBA;
                    decodeOk = true;
                    av_frame_unref(L.swFrame);
                    if (srcFrame == L.nv12Frame) av_frame_unref(L.nv12Frame);
                }
            }
            if (!decodeOk) {
                std::lock_guard<std::shared_mutex> lk(d.mtx);
                d.freeBufs.push_back(std::move(buf));
                continue;
            }
        }

        // P0.2 HAP 路径已由 snappy worker 负责 push ring（含 framesDecoded++），
        // 主线程跳过整个 ring push + framesDecoded 逻辑。
        if (L.codecType == CodecType::HAP && decodeOk) {
            continue;
        }
        {
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            Frame fr;
            fr.gen = d.gen;
            fr.index = nextIndex++;
            fr.fmt = fmt;
            fr.dxtFmt = dxtFmt;
            fr.buf = std::move(buf);
            fr.bufSize = dataSize;
            d.ring.push_back(std::move(fr));
            d.framesDecoded++;
        }
    }
    av_packet_free(&pkt);
}

// P0.2 snappy worker：从 pktQueue 取 HAP packet 做 hap::decodeInto 并 push 到 ring。
// worker 数默认 2（用户要求）；只跑 HAP 层，QTRLE/H.264 不需要（FFmpeg 软解自带多线程）。
// worker 也负责 av_packet_free（packet 由主线程 push 进来）。
static void snappyWorker(Layer& L, int workerId) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // 解码让路，渲染优先
    DecState& d = *L.dec;
    for (;;) {
        DecState::PktJob job{};
        {
            std::unique_lock<std::mutex> lk(d.pktMtx);
            d.pktCv.wait(lk, [&] { return d.workersExit || !d.pktQueue.empty(); });
            if (d.workersExit && d.pktQueue.empty()) break;
            job = d.pktQueue.front();
            d.pktQueue.pop_front();
        }

        // 锁外：取空闲 buf + 解压
        std::vector<uint8_t> buf;
        {
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            if (!d.freeBufs.empty()) {
                buf = std::move(d.freeBufs.back());
                d.freeBufs.pop_back();
            }
        }

        hap::PixFmt dxtFmt = hap::PixFmt::DXT5;
        std::string err;

        // 过期帧快跳（不浪费解码在废帧上）：但只跳"后面还有更新 job"的；
        // 队尾即便过期也照解——解码本来就慢于实时的机器上，全跳会永远追不上（画面冻死）
        if (job.index < d.presentIndex.load(std::memory_order_relaxed) - 2) {
            bool hasFresher;
            {
                std::lock_guard<std::mutex> plk(d.pktMtx);
                hasFresher = !d.pktQueue.empty() && d.pktQueue.back().index > job.index;
            }
            if (hasFresher) {
                av_packet_free(&job.pkt);
                std::lock_guard<std::shared_mutex> lk(d.mtx);
                d.freeBufs.push_back(std::move(buf));
                continue;
            }
        }

        std::vector<uint8_t> buf2;   // Hap Q Alpha 的 BC4 alpha 平面（平时为空）
        bool ok = hap::decodeInto(job.pkt->data, job.pkt->size, buf, dxtFmt, err, &buf2);
        av_packet_free(&job.pkt);

        if (!ok) {
            std::cerr << "[snappy worker " << workerId << "] " << L.name
                      << " 解压失败: " << err << "\n";
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            d.decoderEof = true;
            d.freeBufs.push_back(std::move(buf));
            continue;
        }

        // 推进 ring（持锁）
        {
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            // 主线程可能已退出；exiting 时丢弃已解码帧，避免 push 进 ring 后没人 pop
            if (d.exiting) {
                d.freeBufs.push_back(std::move(buf));
                continue;
            }
            // ring 已满则丢弃（保护：主线程 cv.wait 在控制 ring.size<RING_CAP，
            // 但 worker 并行解压可能瞬时溢出；丢帧比阻塞安全）
            if (d.ring.size() >= RING_CAP) {
                d.freeBufs.push_back(std::move(buf));
                continue;
            }
            Frame fr;
            fr.gen = job.gen;       // B-10 修复：用提交时绑定的序号与轮次
            fr.index = job.index;
            fr.fmt = PixelFormat::DXT;
            fr.dxtFmt = dxtFmt;
            fr.buf = std::move(buf);
            fr.bufSize = fr.buf.size();
            fr.buf2 = std::move(buf2);
            fr.buf2Size = fr.buf2.size();
            // 两个 worker 完成顺序可能乱序：按 index 有序插入，ring 始终保持流内顺序
            auto it = d.ring.begin();
            while (it != d.ring.end() && it->index < fr.index) ++it;
            d.ring.insert(it, std::move(fr));
            d.framesDecoded++;
            d.cv.notify_one();  // 唤醒主线程 pop
        }
    }
}

// snappy worker 数量：默认 1。
// 实测（6800H）：每路 4K30 的 snappy 解码单线程已够；开 2 个 worker 时 10 层=20 个
// 工作线程在 8 核上互相争抢 + 锁 convoy，fps 从 ~55 崩到个位数。大分辨率单路时可调回 2。
static constexpr int kSnappyWorkerCount = 1;

static void startDecoder(Layer& L) {
    if (!L.dec) L.dec = std::make_unique<DecState>();
    if (!L.dec->th.joinable())
        L.dec->th = std::thread(decoderRun, std::ref(L));
    // P0.2：仅 HAP 层启动 snappy workers（QTRLE/H.264 走 FFmpeg 软解，多线程收益小）
    if (L.codecType == CodecType::HAP) {
        bool expected = false;
        if (L.dec->workersStarted.compare_exchange_strong(expected, true)) {
            L.dec->workers.reserve(kSnappyWorkerCount);
            for (int i = 0; i < kSnappyWorkerCount; ++i) {
                L.dec->workers.emplace_back(snappyWorker, std::ref(L), i);
            }
        }
    }
}

static void stopDecoder(Layer& L) {
    if (!L.dec) return;
    // 先关闭主线程，避免它继续往 pktQueue push
    {
        std::lock_guard<std::shared_mutex> lk(L.dec->mtx);
        L.dec->exiting = true;
    }
    L.dec->cv.notify_all();
    // 关闭 snappy workers：清空 pktQueue（避免 worker 处理已无主的 packet）+ 唤醒
    {
        std::lock_guard<std::mutex> lk(L.dec->pktMtx);
        L.dec->workersExit = true;
        // 排空残留 pkt，避免 worker 处理已退出的主线程遗留的 packet
        for (auto& job : L.dec->pktQueue) av_packet_free(&job.pkt);
        L.dec->pktQueue.clear();
    }
    L.dec->pktCv.notify_all();
    // join 主线程
    if (L.dec->th.joinable()) L.dec->th.join();
    // join workers
    for (auto& w : L.dec->workers) {
        if (w.joinable()) w.join();
    }
    L.dec->workers.clear();
    L.dec->workersStarted = false;
    L.dec->workersExit = false;
}

// 全局音频配置（在 loadConfig 里填充，由 main 用 audio::Context::init() 加载）
static audio::Config g_audioCfg;
static std::string g_audioCfgSource;     // loadConfig 后由 main 在 init 前解析绝对路径

// 小项 2：日志级别（0=error, 1=warn, 2=info, 3=debug），默认 info。
// 替换关键 std::cerr 调用 → LOG(level, msg) 宏，level 大于 g_logLevel 不输出。
// 放在 loadConfig 之前，loadConfig 内部也会用
static int  g_logLevel = 2;
#define LOG(level, msg) do { if ((level) <= g_logLevel) std::cerr << msg; } while (0)

// 小项 1：JSON bench 报告（CI/CD 集成用）。默认关闭，命令行 --json 打开。
static bool g_jsonBench = false;
// 小项 4：配置字段校验。loadConfig 自增，bench 结束 / 退出时打印汇总。
static int  g_configErrors = 0;

// 资源预加载配置（[preload] 段）
static bool g_preloadEnabled = true;        // 默认开；0/1 关闭
static double g_preloadMaxDuration = 30.0;  // < 此秒数的层预加载
static bool g_preloadInited = false;        // 是否已经解析过 [preload] 段

// 预加载：对非 HAP 层一次性解完所有帧到内存（消除冷启动卡顿）
// 仅对 < g_preloadMaxDuration 秒生效。HAP 不需要（packet 直接拿 DXT）。
// 失败时 L.preloaded = false，不影响 decoderRun 走实时解码路径。
static void preloadLayer(Layer& L) {
    if (!g_preloadEnabled) return;
    if (L.codecType == CodecType::HAP) return;            // HAP 走压缩纹理直接读 packet
    if (!(L.duration > 0.0 && L.duration < g_preloadMaxDuration)) return;
    if (!L.codecCtx || !L.swsCtx || !L.rgbBuf || !L.rgbFrame || !L.swFrame) {
        std::cerr << "[预加载] " << L.name << ": 解码器资源不齐，跳过\n";
        return;
    }
    // 跳到文件开始、flush decoder
    if (av_seek_frame(L.fmt, L.streamIdx, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        std::cerr << "[预加载] " << L.name << ": seek 失败，跳过\n";
        return;
    }
    avformat_flush(L.fmt);
    if (L.codecCtx) avcodec_flush_buffers(L.codecCtx);

    AVPacket* pkt = av_packet_alloc();
    int64_t totalFrames = (int64_t)std::lround(L.duration * L.fps);
    if (totalFrames <= 0) totalFrames = 1;
    L.preloadedFrames.reserve((size_t)totalFrames);

    int framesDone = 0;
    int eof = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (!eof && (int64_t)L.preloadedFrames.size() < totalFrames) {
        int ret = av_read_frame(L.fmt, pkt);
        if (ret < 0) { eof = 1; break; }
        if (pkt->stream_index != L.streamIdx) { av_packet_unref(pkt); continue; }
        int sr = avcodec_send_packet(L.codecCtx, pkt);
        av_packet_unref(pkt);
        if (sr < 0) continue;
        while (avcodec_receive_frame(L.codecCtx, L.swFrame) == 0) {
            AVFrame* srcFrame = L.swFrame;
            bool skip = false;
            if (L.useNVDEC && L.swFrame->format == AV_PIX_FMT_CUDA) {
                av_frame_unref(L.nv12Frame);
                if (av_hwframe_transfer_data(L.nv12Frame, L.swFrame, 0) == 0) {
                    srcFrame = L.nv12Frame;
                } else {
                    // B-6 修复：transfer 失败不能把 CUDA 显存帧喂给 sws_scale，跳过该帧
                    skip = true;
                }
            }
            if (skip) { av_frame_unref(L.swFrame); continue; }
            sws_scale(L.swsCtx,
                      (const uint8_t* const*)srcFrame->data, srcFrame->linesize,
                      0, L.h, L.rgbFrame->data, L.rgbFrame->linesize);
            // 拷贝这一帧到独立 vector
            std::vector<uint8_t> frame(L.rgbBuf, L.rgbBuf + L.rgbBufSize);
            L.preloadedFrames.push_back(std::move(frame));
            framesDone++;
            av_frame_unref(L.swFrame);
            if (srcFrame == L.nv12Frame) av_frame_unref(L.nv12Frame);
        }
    }
    av_packet_free(&pkt);

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!L.preloadedFrames.empty()) {
        L.preloaded = true;
        size_t bytes = L.preloadedFrames.size() * (size_t)L.w * (size_t)L.h * 4;
        std::cout << "[预加载] " << L.name << ": " << L.preloadedFrames.size()
                  << " 帧 / " << (bytes / 1024 / 1024) << " MB / "
                  << (int)ms << " ms (" << (int)(1000.0 * framesDone / ms) << " fps)\n";
    } else {
        std::cerr << "[预加载] " << L.name << ": 0 帧，跳过\n";
    }
}

static void closeLayer(Layer& L) {
    stopDecoder(L);
    if (L.fmt) avformat_close_input(&L.fmt);
    if (L.tex) glDeleteTextures(1, &L.tex);
    if (L.texA) glDeleteTextures(1, &L.texA);
#ifdef HAPPLAYER_HAS_VULKAN
    // Vulkan 阶段 E：销毁该层纹理（含持久 staging）。Renderer 仍存活时才能销毁。
    if (L.vkTex.image != VK_NULL_HANDLE && vk_render::Renderer::instance().isInitialized()) {
        vk_hapq::destroyDXT(vk_render::Renderer::instance().getDevice(), L.vkTex);
        L.vkDirty = false;
    }
#endif
    // 预加载帧清理
    L.preloadedFrames.clear();
    L.preloadedFrames.shrink_to_fit();
    L.preloaded = false;
    // P0.1 PBO 清理（仅 PBO 可用时分配过，函数指针判空兜底）
    if (g_pboAvailable) {
        for (int i = 0; i < 2; ++i) {
            if (L.pboFence[i]) { pglDeleteSync(L.pboFence[i]); L.pboFence[i] = nullptr; }
        }
        if (L.pbo[0]) { pglDeleteBuffers(2, L.pbo); L.pbo[0] = L.pbo[1] = 0; }
    }
    // 多 codec 软解资源清理（QTRLE / H264 路径）
    if (L.swsCtx)   { sws_freeContext(L.swsCtx); L.swsCtx = nullptr; }
    if (L.rgbBuf)   { av_free(L.rgbBuf);         L.rgbBuf = nullptr; }
    if (L.rgbFrame) { av_frame_free(&L.rgbFrame); }
    if (L.swFrame)  { av_frame_free(&L.swFrame); }
    if (L.codecCtx) { avcodec_free_context(&L.codecCtx); }
    // P2.3 NVDEC 资源清理（仅 H.264 + useNVDEC 时存在）
    if (L.hwFrame)    { av_frame_free(&L.hwFrame); }
    if (L.nv12Frame)  { av_frame_free(&L.nv12Frame); }
    if (L.hw_device_ctx) { av_buffer_unref(&L.hw_device_ctx); }
    L.rgbBufSize = 0;
    L = Layer{};
}

// 队列（重）开始时复位图层到待播状态并激活解码
// 注意：不要重置 L.visible=false（保留纹理旧帧可避免切场瞬间黑屏）；
//       updateLayer 会按 start 时间 / texAllocated 自动控制 visible。
static void resetLayer(Layer& L) {
    {
        std::lock_guard<std::shared_mutex> lk(L.dec->mtx);
        L.dec->pendingReset = true;
        L.dec->active = true;
        // 切场/重播时同步播放头闸门：否则闸门残留上轮末尾值（如 119），
        // 解码线程会一口气读到 EOF 然后挂起，新一轮无帧可播（实测卡死根因）
        L.dec->presentIndex.store(0, std::memory_order_relaxed);
    }
    L.dec->cv.notify_all();
    L.shownIndex = -1;
    L.prevLt = -1.0;
    L.state = "waiting";
}

// 队列切走后停用：解码线程挂起，不再吃 CPU/磁盘
static void deactivateLayer(Layer& L) {
    {
        std::lock_guard<std::shared_mutex> lk(L.dec->mtx);
        L.dec->active = false;
    }
    L.dec->cv.notify_all();
    L.visible = false;
}

// 健康监控计数器（全局区有注释说明；定义放这里是因为 preRoll 要用）
static double g_fpsBelowSince = 0;          // fps < 5 起始时间（0 = 未触发）

// 预读：等快入场的层都缓冲好再开走队列时钟（避免开播/切场瞬间卡顿）
static void preRoll(Queue& q, double maxWaitSec) {
    // 4K 高码率下 RING_CAP=12 填满需要更多时间，默认 5 秒更保险
    double deadline = glfwGetTime() + maxWaitSec;
    for (;;) {
        bool ready = true;
        for (auto& L : q.layers) {
            if (L.start > 1.0) continue;   // 入场晚的层还有时间慢慢解码
            DecState& d = *L.dec;
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            // 必须缓冲到【当前轮次】的帧：切场瞬间 ring 里可能是上一轮残留，
            // 旧帧满足数量条件会让时钟提前开走（帧被 gen 丢弃后无帧可播）
            bool freshEnough = d.ring.size() >= 2 && d.ring.front().gen == d.gen;
            if (!freshEnough) ready = false;   // 旧轮残留/未缓冲够都等（超时兜底）
        }
        if (ready) break;
        if (glfwGetTime() > deadline) {
            std::cerr << "[警告] 预读超时，直接开播\n";
            break;
        }
        glfwPollEvents();   // 保持窗口响应
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // B-14 修复：预读阻塞期间 fps 统计失真，复位健康监控避免误杀
    g_fpsBelowSince = 0;
}

// P0.1: PBO 异步纹理上传（DXT 压缩纹理专用）
// 首次分配（texAllocated=false）仍同步 Image2D；之后 SubImage 用 PBO + fence 异步
//   1. 等待这个 PBO 上一次上传的 fence 完成（避免读写冲突）
//   2. glBufferData 把 DXT 数据写入 PBO（驱动可走 DMA 异步）
//   3. glCompressedTexSubImage2D 从 PBO 异步读到纹理（CPU 立即返回）
//   4. 记录 fence，下次 upload 时同步
// 收益：15 层4K 主线程不再等 GPU 上传完，帧时间从 ~125ms 降到 ~50-60ms
//
// RGBA 路径（H.264 / QTRLE）：与 DXT 共用同一组 PBO + fence
//   - 首次分配：glTexImage2D(GL_RGBA8) + 立 PBO；后续帧全走 PBO 异步
//   - PBO 写入和上传代码与 DXT 完全相同，只在 3) 步按 texIsRGBA 分支调用不同的 SubImage
//   - 4K RGBA 8MB/帧 × 30fps = 240MB/s 走 PBO 后主线程不再阻塞
//
// 输入：原始字节由 data/size 传入；调用方负责把 Frame.buf 数据传入
static void upload(Layer& L, const uint8_t* data, size_t size,
                   PixelFormat fmt, hap::PixFmt dxtFmt,
                   const uint8_t* data2 = nullptr, size_t size2 = 0) {
    // HAP Q Alpha：第二平面 BC4 alpha → L.texA（量小，同步上传即可）
    if (data2 && size2 > 0) {
        if (!L.texA) {
            glGenTextures(1, &L.texA);
            glBindTexture(GL_TEXTURE_2D, L.texA);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            pglCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1,
                                    L.w, L.h, 0, (GLsizei)size2, data2);
        } else {
            glBindTexture(GL_TEXTURE_2D, L.texA);
            pglCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.w, L.h,
                                       GL_COMPRESSED_RED_RGTC1, (GLsizei)size2, data2);
        }
    }
#ifdef HAPPLAYER_HAS_VULKAN
    // Vulkan 阶段 E：HAPPLAYER_USE_VULKAN 启用时跳过 OpenGL 上传
    // HAP Q / DXT1 / DXT5 → BC1/BC3；RGBA（H.264 / QTRLE）→ R8G8B8A8
    // CPU 只 memcpy 到持久 staging；GPU copy 在 drawFrame 的 render pass 之前完成
    static thread_local bool s_vkLogOnce = false;
    if (g_useVulkan && vk_render::Renderer::instance().isInitialized()) {
        // Hap Q Alpha 的第二平面（BC4 alpha）Vulkan 路径暂不支持：告警一次并忽略
        if (data2 && size2 > 0) {
            static bool s_warnedHapQA = false;
            if (!s_warnedHapQA) {
                std::cerr << "[vulkan] [警告] Hap Q Alpha 的 alpha 平面在 Vulkan 路径暂不支持"
                             "（按不透明处理）；需要透明请用 OpenGL 路径（默认）\n";
                s_warnedHapQA = true;
            }
        }
        VkFormat vkFmt = VK_FORMAT_UNDEFINED;
        if (fmt == PixelFormat::RGBA) {
            vkFmt = VK_FORMAT_R8G8B8A8_UNORM;
        } else if (dxtFmt == hap::PixFmt::DXT1) {
            vkFmt = VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        } else {
            vkFmt = VK_FORMAT_BC3_UNORM_BLOCK;
        }
        auto& R = vk_render::Renderer::instance();
        if (L.vkTex.image == VK_NULL_HANDLE) {
            L.vkTex = vk_hapq::createTexture(R.getDevice(), R.getPhysDevice(),
                                             data, size, (uint32_t)L.w, (uint32_t)L.h, vkFmt);
            if (L.vkTex.image == VK_NULL_HANDLE) {
                if (!s_vkLogOnce) {
                    std::cerr << "[vulkan] createTexture 失败（HAPPLAYER_USE_VULKAN 路径将持续黑屏）\n";
                    s_vkLogOnce = true;
                }
                return;
            }
            if (!s_vkLogOnce) {
                std::cerr << "[vulkan] 纹理创建成功 fmt=" << (int)vkFmt
                          << " " << L.w << "x" << L.h << " size=" << size << " B\n";
                s_vkLogOnce = true;
            }
        } else if (!vk_hapq::updateTexture(L.vkTex, data, size)) {
            return;
        }
        L.vkDirty = true;
        return;
    }
#endif

    glBindTexture(GL_TEXTURE_2D, L.tex);

    // 解析格式参数（DXT 才有 dxtFmt/internal；RGBA 用固定参数）
    const bool isRGBA = (fmt == PixelFormat::RGBA);
    GLenum internal = (!isRGBA)
        ? ((dxtFmt == hap::PixFmt::DXT1)
              ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT
              : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
        : 0;
    const GLsizei isize = (GLsizei)size;

    // ============ 首次分配（同步 + 建 PBO）============
    if (!L.texAllocated) {
        if (isRGBA) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, L.w, L.h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, data);
            L.texIsRGBA = true;
        } else {
            pglCompressedTexImage2D(GL_TEXTURE_2D, 0, internal, L.w, L.h, 0, isize, data);
            L.texIsRGBA = false;
        }
        // 顺便分配 PBO（按当前帧大小取整，避免反复 realloc）
        if (g_pboAvailable) {
            L.pboCap = (size_t)isize;
            pglGenBuffers(2, L.pbo);
            for (int i = 0; i < 2; ++i) {
                pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, L.pbo[i]);
                pglBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizei)L.pboCap, nullptr, GL_STREAM_DRAW);
            }
            pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        L.texAllocated = true;
        return;
    }

    // PBO 不可用 → 同步上传（老驱动/远程桌面兜底路径）
    if (!g_pboAvailable) {
        if (L.texIsRGBA) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.w, L.h,
                            GL_RGBA, GL_UNSIGNED_BYTE, data);
        } else {
            pglCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.w, L.h, internal, isize, data);
        }
        return;
    }

    // 帧大小变化（罕见但要兜住）→ 重建 PBO
    if ((size_t)isize > L.pboCap) {
        if (L.pboFence[0]) { pglDeleteSync(L.pboFence[0]); L.pboFence[0] = nullptr; }
        if (L.pboFence[1]) { pglDeleteSync(L.pboFence[1]); L.pboFence[1] = nullptr; }
        pglDeleteBuffers(2, L.pbo);
        L.pboCap = (size_t)isize;
        pglGenBuffers(2, L.pbo);
        for (int i = 0; i < 2; ++i) {
            pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, L.pbo[i]);
            pglBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizei)L.pboCap, nullptr, GL_STREAM_DRAW);
        }
    }

    // 双缓冲 ping-pong
    const int idx = L.pboIdx;
    L.pboIdx = 1 - idx;

    // 1) 等这个 PBO 上一次上传完成（GPU 读完了才能再写）
    if (L.pboFence[idx]) {
        // B-17 修复：100ms 上限，GPU 卡死时主线程不陪葬，超时就当已完成继续走
        GLenum wr = pglClientWaitSync(L.pboFence[idx], 0, 100000000);
        if (wr == 0x911B /*GL_TIMEOUT_EXPIRED*/) {
            static int fenceTimeoutCount = 0;
            if (++fenceTimeoutCount % 60 == 1)
                std::cerr << "[警告] PBO fence 等待超时（GPU 繁忙？），已发生 "
                          << fenceTimeoutCount << " 次\n";
        }
        pglDeleteSync(L.pboFence[idx]);
        L.pboFence[idx] = nullptr;
    }

    // 2) 把数据写入 PBO（驱动可走 DMA 异步上传）
    pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, L.pbo[idx]);
    pglBufferData(GL_PIXEL_UNPACK_BUFFER, isize, nullptr, GL_STREAM_DRAW);  // 先 orphan 旧数据
    void* mapped = pglMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, isize,
                                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (mapped) {
        std::memcpy(mapped, data, isize);
        pglUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    } else {
        // 退化路径：映射失败，CPU memcpy 直接走（同步）
        pglBufferData(GL_PIXEL_UNPACK_BUFFER, isize, data, GL_STREAM_DRAW);
    }

    // 3) 异步上传：data 参数传 nullptr → 从 PBO 异步读
    if (L.texIsRGBA) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.w, L.h,
                        GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    } else {
        pglCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.w, L.h, internal, isize, nullptr);
    }
    pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // 4) 记录 fence，下次 upload 时同步
    L.pboFence[idx] = pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

// 按队列时钟推进图层状态：从预读环里取帧
static void updateLayer(Layer& L, double t) {
    double local = t - L.start;
    if (local < 0.0) {
        L.visible = false; L.state = "waiting";
        if (L.audioStreamIdx >= 0)
            audio::Context::instance().setSourcePlaying(L.path, false);
        return;
    }

    double lt;
    bool wrapped = false;
    if (L.loop && L.duration > 0.0) {
        lt = std::fmod(local, L.duration);
        if (L.prevLt >= 0.0 && lt < L.prevLt - 1e-3)
            wrapped = true;   // 时钟回卷：通知解码线程 seek 回 0（gen++，旧轮帧按 gen 丢弃）
    } else {
        if (L.duration > 0.0 && local >= L.duration) {
            L.visible = false; L.state = "ended"; L.prevLt = -1.0;
            if (L.audioStreamIdx >= 0)
                audio::Context::instance().setSourcePlaying(L.path, false);
            return;
        }
        lt = local;
    }
    L.prevLt = lt;

    int64_t want = (int64_t)std::floor(lt * L.fps + 1e-6);

    DecState& d = *L.dec;
    d.presentIndex.store(want, std::memory_order_relaxed);   // 告诉解码线程：只允许领先 RING_CAP 帧
    if (wrapped) {
        {
            std::lock_guard<std::shared_mutex> lk(d.mtx);
            d.pendingReset = true;
        }
        d.cv.notify_all();
    }

    Frame picked;
    bool have = false;
    {
        std::lock_guard<std::shared_mutex> lk(d.mtx);
        const uint64_t g = d.gen;

        // 跳过过期轮次的帧
        while (!d.ring.empty() && d.ring.front().gen != g) {
            d.freeBufs.push_back(std::move(d.ring.front().buf));
            d.ring.pop_front();
        }
        // 只保留「≤ want 的最新一帧」：更旧的丢弃（晚了也显示，宁可微滞后不可冻屏）
        while (d.ring.size() >= 2) {
            const Frame& nextF = d.ring[1];
            if (nextF.gen == g && nextF.index <= want) {
                d.freeBufs.push_back(std::move(d.ring.front().buf));
                d.ring.pop_front();
            } else {
                break;
            }
        }
        // 候选帧：主线程用 std::move 拿数据
        if (!d.ring.empty() && d.ring.front().gen == g && d.ring.front().index <= want) {
            picked.buf = std::move(d.ring.front().buf);
            picked.bufSize = d.ring.front().bufSize;
            picked.fmt = d.ring.front().fmt;
            picked.dxtFmt = d.ring.front().dxtFmt;
            picked.gen = d.ring.front().gen;
            picked.index = d.ring.front().index;
            picked.sharedBuf = d.ring.front().sharedBuf;
            picked.buf2 = std::move(d.ring.front().buf2);
            picked.buf2Size = d.ring.front().buf2Size;
            d.ring.pop_front();
            have = true;
        }
    }
    d.cv.notify_one();

    if (have) {
        const std::vector<uint8_t>& upData = picked.sharedBuf ? *picked.sharedBuf : picked.buf;
        upload(L, upData.data(), picked.bufSize, picked.fmt, picked.dxtFmt,
               picked.buf2.empty() ? nullptr : picked.buf2.data(), picked.buf2Size);
        if (picked.fmt == PixelFormat::DXT && picked.dxtFmt == hap::PixFmt::YCoCg_DXT5)
            L.isHapQ = true;
        L.shownIndex = picked.index;
        std::lock_guard<std::shared_mutex> lk(d.mtx);
        d.freeBufs.push_back(std::move(picked.buf));
    }
#ifdef HAPPLAYER_HAS_VULKAN
    L.visible = (g_useVulkan && vk_render::Renderer::instance().isInitialized())
        ? (L.vkTex.view != VK_NULL_HANDLE)
        : L.texAllocated;
#else
    L.visible = L.texAllocated;
#endif
    L.state = "playing";
    if (L.audioStreamIdx >= 0)
        audio::Context::instance().setSourcePlaying(L.path, true);
}

// 场间淡入淡出系数（1 = 正常显示；0 = 全黑）。主循环淡变状态机驱动
static double g_fadeAlpha = 1.0;

// contain 适配居中（默认）或自定义矩形（图层行 |x%|y%|宽% 字段），再按 scale 缩放
static void drawLayer(const Layer& L, int winW, int winH, double scale) {
    if (!L.visible || !L.texAllocated) return;
    double va = (double)L.w / L.h;
    double rw, rh, x0, y0;
    if (L.customRect) {
        // 自定义矩形：x/y/宽 占窗口百分比，高度按比例，scale 以矩形中心缩放
        double rw0 = winW * (L.rw / 100.0);
        double rh0 = rw0 / va;
        rw = rw0 * scale; rh = rh0 * scale;
        x0 = winW * (L.rx / 100.0) + (rw0 - rw) * 0.5;
        y0 = winH * (L.ry / 100.0) + (rh0 - rh) * 0.5;
    } else {
        double wa = (double)winW / winH;
        if (va > wa) { rw = winW; rh = winW / va; }
        else         { rh = winH; rw = winH * va; }
        rw *= scale; rh *= scale;
        x0 = (winW - rw) * 0.5; y0 = (winH - rh) * 0.5;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, L.tex);

    // HAP Q / HAP Q Alpha：DXT5 采样到的是 YCoCg，必须用 shader 转 RGB
    if (L.isHapQ && g_hapQProgram) {
        const bool withAlpha = (L.texA != 0 && g_hapQAProgram != 0);
        pglUseProgram(withAlpha ? g_hapQAProgram : g_hapQProgram);
        if (withAlpha) {
            // 颜色 → 纹理单元 0，BC4 alpha 平面 → 纹理单元 1
            pglActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, L.texA);
            pglActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, L.tex);
            pglUniform1i(g_hapQAUTex, 0);
            pglUniform1i(g_hapQAUTexA, 1);
            if (g_hapQAUAlpha >= 0) pglUniform1f(g_hapQAUAlpha, (float)g_fadeAlpha);
        } else {
            pglUniform1i(g_hapQUTex, 0);   // GL_TEXTURE0
            if (g_hapQUAlpha >= 0) pglUniform1f(g_hapQUAlpha, (float)g_fadeAlpha);
        }
        // y 轴翻转：固定管线路径靠 glOrtho(0,w,h,0) 把 y0(顶) 映到 NDC +1，
        // shader 路径手动换算必须一致：ny = 1 - y/h*2（否则 HAP Q 层垂直镜像）
        const float nx0 = (float)(x0 / winW * 2.0 - 1.0);
        const float nx1 = (float)((x0 + rw) / winW * 2.0 - 1.0);
        const float ny0 = (float)(1.0 - y0 / winH * 2.0);          // 顶 → NDC +1
        const float ny1 = (float)(1.0 - (y0 + rh) / winH * 2.0);   // 底 → NDC -1
        // attribute 写入顺序：aPos 绑定在 location 0（见 initShaders），
        // spec 规定 Begin/End 内写 location 0 才触发顶点发射——
        // 必须先写 aUV 再写 aPos，否则 UV 错位一个顶点（画面旋转/镜像）
        glBegin(GL_QUADS);
        pglVertexAttrib2f(g_hapQAttrUV, 0.f, 0.f); pglVertexAttrib2f(g_hapQAttrPos, nx0, ny0);
        pglVertexAttrib2f(g_hapQAttrUV, 1.f, 0.f); pglVertexAttrib2f(g_hapQAttrPos, nx1, ny0);
        pglVertexAttrib2f(g_hapQAttrUV, 1.f, 1.f); pglVertexAttrib2f(g_hapQAttrPos, nx1, ny1);
        pglVertexAttrib2f(g_hapQAttrUV, 0.f, 1.f); pglVertexAttrib2f(g_hapQAttrPos, nx0, ny1);
        glEnd();
        if (withAlpha) {
            pglActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            pglActiveTexture(GL_TEXTURE0);
        }
        pglUseProgram(0);
    } else {
        // 固定管线：MODULATE 把 g_fadeAlpha 乘进 alpha（淡入淡出）
        glColor4f(1.f, 1.f, 1.f, (float)g_fadeAlpha);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2d(x0,      y0);
        glTexCoord2f(1, 0); glVertex2d(x0 + rw, y0);
        glTexCoord2f(1, 1); glVertex2d(x0 + rw, y0 + rh);
        glTexCoord2f(0, 1); glVertex2d(x0,      y0 + rh);
        glEnd();
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }
}

// ---------------- 配置 ----------------
static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 相对路径 → 基于配置文件所在目录的绝对/完整路径
static std::string resolvePath(const std::string& configDir, const std::string& p) {
    bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
    if (absolute || configDir.empty()) return p;
    return configDir + "/" + p;
}

static std::vector<Queue> loadConfig(const std::string& path, WindowCfg& win) {
    std::vector<Queue> queues;
    std::ifstream f(path);
    // 小项 4：配置文件不存在 → 返回错误码（之前是静默返回空 queues，被 main 当成"无可用队列"误判）
    if (!f) { LOG(0, "[错误] 打不开配置文件: " << path << "\n"); return queues; }
    // 配置文件所在目录，视频相对路径以它为基准
    std::string configDir;
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) configDir = path.substr(0, slash);
    std::string line;
    int lineno = 0;
    bool inWindow = false;
    bool inAudio  = false;
    bool inPreload = false;              // [preload] 段状态（避免误判其他段的 key=value 行）
    std::string audioSourceRel;          // [audio] source 解析前的相对路径
    while (std::getline(f, line)) {
        lineno++;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "[window]") { inWindow = true; inAudio = false; inPreload = false; continue; }
        if (line == "[audio]") {
            // 单音频配置段（跟随队列时钟播放；HAP 不带音频，必须外挂）
            inWindow = false;
            inAudio  = true;
            inPreload = false;
            std::cout << "[audio]\n";
            continue;
        }
        if (line == "[preload]") {
            // 资源预加载：< max_duration 秒的非 HAP 层一次性解到内存
            inWindow = false;
            inAudio  = false;
            inPreload = true;
            g_preloadInited = true;
            std::cout << "[preload]\n";
            continue;
        }
        if (line == "[queue]") {
            inWindow = false;
            inAudio  = false;
            inPreload = false;
            queues.emplace_back();
            std::cout << "[队列 " << queues.size() << "]\n";
            continue;
        }
        if (inWindow) {
            // 行格式: key=value
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                LOG(1, "[配置] 第 " << lineno << " 行格式不对，已跳过: " << line << "\n");
                g_configErrors++;
                continue;
            }
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (key == "size") {
                if (std::sscanf(val.c_str(), "%dx%d", &win.w, &win.h) != 2 || win.w <= 0 || win.h <= 0) {
                    LOG(1, "[配置] size 格式应为 宽x高（当前: " << val << "），使用默认 1280x720\n");
                    g_configErrors++;
                    win.w = 1280; win.h = 720;
                }
            }
            else if (key == "pos") {
                if (std::sscanf(val.c_str(), "%d%*[,xX]%d", &win.x, &win.y) != 2) {
                    LOG(1, "[配置] pos 格式应为 x,y（当前: " << val << "），使用默认 0,0\n");
                    g_configErrors++;
                    win.x = 0; win.y = 0;
                }
            }
            else if (key == "monitor")    win.monitor = std::atoi(val.c_str());
            else if (key == "fade")       win.fade = std::atof(val.c_str());
            else if (key == "scale") {
                int s = std::atoi(val.c_str());
                if (s < 10 || s > 400) {
                    LOG(1, "[配置] scale 应在 10~400（当前: " << s << "），使用默认 100\n");
                    g_configErrors++;
                } else win.scale = s;
            }
            else if (key == "fullscreen") win.fullscreen = std::atoi(val.c_str()) != 0;
            else if (key == "borderless") win.borderless = std::atoi(val.c_str()) != 0;
            else { LOG(1, "[配置] 未知窗口配置项: " << key << "\n"); g_configErrors++; }
            continue;
        }
        if (inAudio) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                LOG(1, "[配置] [audio] 第 " << lineno << " 行格式不对，已跳过: " << line << "\n");
                g_configErrors++;
                continue;
            }
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if      (key == "source") audioSourceRel = val;
            else if (key == "volume") {
                float v = (float)std::atof(val.c_str());
                if (v < 0.0f || v > 1.0f) {
                    LOG(1, "[配置] [audio] volume 应在 0.0~1.0（当前: " << v << "），使用默认 1.0\n");
                    g_configErrors++;
                    g_audioCfg.volume = 1.0f;
                } else g_audioCfg.volume = v;
            }
            else if (key == "loop")   g_audioCfg.loop   = std::atoi(val.c_str()) != 0;
            else { LOG(1, "[配置] [audio] 未知键: " << key << "\n"); g_configErrors++; }
            continue;
        }
        if (inPreload) {
            // [preload] 段的 key=value 行（用 inPreload 状态位判断）
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if      (key == "enabled")      g_preloadEnabled = std::atoi(val.c_str()) != 0;
            else if (key == "max_duration") g_preloadMaxDuration = std::atof(val.c_str());
            else std::cerr << "[警告] [preload] 未知键: " << key << "\n";
            continue;
        }
        if (queues.empty()) queues.emplace_back(); // 没写 [queue] 就全部算一个队列
        // 行格式: 开始秒|循环(0/1)|路径
        size_t p1 = line.find('|');
        size_t p2 = (p1 == std::string::npos) ? p1 : line.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) {
            LOG(1, "[配置] 第 " << lineno << " 行格式不对，已跳过: " << line << "\n");
            g_configErrors++;
            continue;
        }
        std::string startStr = trim(line.substr(0, p1));
        std::string loopStr  = trim(line.substr(p1 + 1, p2 - p1 - 1));
        // 小项 4：开始秒必须 ≥ 0，循环必须是 0 或 1
        double startSec = std::atof(startStr.c_str());
        if (startSec < 0.0) {
            LOG(1, "[配置] 第 " << lineno << " 行开始秒 < 0（当前: " << startSec << "），已跳过\n");
            g_configErrors++;
            continue;
        }
        int loopVal = std::atoi(loopStr.c_str());
        if (loopStr != "0" && loopStr != "1") {
            LOG(1, "[配置] 第 " << lineno << " 行循环标志必须为 0 或 1（当前: " << loopStr << "），已跳过\n");
            g_configErrors++;
            continue;
        }
        Layer L;
        L.start = startSec;
        L.loop  = (loopVal != 0);
        // 可选扩展字段: 路径|x%|y%|宽%|音量%（x,y = 矩形左上角占窗口百分比；宽占比，高按比例）
        std::string rest = line.substr(p2 + 1);
        size_t p3 = rest.find('|');
        if (p3 == std::string::npos) {
            L.path = resolvePath(configDir, trim(rest));
        } else {
            L.path = resolvePath(configDir, trim(rest.substr(0, p3)));
            std::vector<std::string> ext;
            std::string cur;
            for (size_t i = p3 + 1; i <= rest.size(); ++i) {
                if (i == rest.size() || rest[i] == '|') { ext.push_back(trim(cur)); cur.clear(); }
                else cur += rest[i];
            }
            if (ext.size() >= 3 && !ext[0].empty() && !ext[1].empty() && !ext[2].empty()) {
                L.customRect = true;
                L.rx = (float)std::atof(ext[0].c_str());
                L.ry = (float)std::atof(ext[1].c_str());
                L.rw = (float)std::atof(ext[2].c_str());
            }
            if (ext.size() >= 4 && !ext[3].empty())
                L.volume = (float)std::atof(ext[3].c_str()) / 100.0f;
        }
        if (openLayer(L)) queues.back().layers.push_back(std::move(L));
        else closeLayer(L);
    }
    // 清掉空队列；检查每个队列是否有不循环层（否则队列永不结束）
    std::vector<Queue> ok;
    for (size_t i = 0; i < queues.size(); ++i) {
        if (queues[i].layers.empty()) continue;
        bool hasNonLoop = false;
        for (const auto& L : queues[i].layers) if (!L.loop) hasNonLoop = true;
        if (!hasNonLoop) {
            LOG(1, "[配置] 队列 " << (i + 1) << " 全是循环层，永远不会切到下一队列\n");
            g_configErrors++;
        }
        ok.push_back(std::move(queues[i]));
    }
    // 小项 4：汇总打印（只在有错误时显示，避免噪音）
    if (g_configErrors > 0) {
        LOG(1, "[配置] 共 " << g_configErrors << " 处警告（详见上方）\n");
    }
    // 把 audio 源解析成绝对路径（main 调用 init 前用）
    if (!audioSourceRel.empty()) {
        g_audioCfg.source = resolvePath(configDir, audioSourceRel);
    }
    return ok;
}

#ifdef HAPPLAYER_HAS_VULKAN
// Vulkan 路径的淡出黑场纹理（1x1 RGBA，每帧改 alpha 值实现淡入淡出——
// SPIR-V 是手工汇编的没法加 uniform，用顶层半透明黑 Quad 等价实现）
static vk_hapq::DXTImage s_vkFadeTex;
#endif

// ---------------- 全局状态（按键/UDP 控制用） ----------------
static bool  g_paused = false;
static bool  g_restart = false;
static int   g_queueStep = 0;    // +1 下一队列 / -1 上一队列
static int   g_queueJump = -1;   // 跳到指定队列（0 起）
static bool  g_fullscreen = false;
static GLFWwindow* g_win = nullptr;
static GLFWmonitor* g_targetMonitor = nullptr; // F 键全屏用的目标显示器
static int g_winX = 100, g_winY = 100, g_winW = 1280, g_winH = 720;
static double g_maxFps = 60.0;   // 自定义 frame pacing 上限（默认 60，命令行 --max-fps 覆盖）

// 配置热重载：记录 layers.txt 的 mtime，主循环每 1 秒轮询变化
static time_t g_configMtime = 0;          // 启动时的基准 mtime
static double g_nextHotReloadCheck = 0;    // 下次检查时间（基于 glfwGetTime）
// 健康监控：检测 fps 持续过低 + 启动时间太长，主动退出等外部进程拉起
static std::chrono::steady_clock::time_point g_startTime = std::chrono::steady_clock::now();
// g_fpsBelowSince 在 preRoll 之前已定义（预读结束要复位它，防止误杀）

// 小项 3：窗口位置记忆 — 退出时存 %APPDATA%/happlayer/state.json，下次启动读
// 用 std::wstring 调 SHGetFolderPathW 拿 APPDATA 路径，避免编码歧义
static std::string getStateFilePath() {
    WCHAR appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return "";
    char mb[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, appdata, -1, mb, sizeof(mb), nullptr, nullptr);
    std::string dir = std::string(mb) + "\\happlayer";
    CreateDirectoryA(dir.c_str(), nullptr);  // 已存在也 ok
    return dir + "\\state.json";
}

static bool loadWindowState(int& x, int& y, int& w, int& h) {
    std::string p = getStateFilePath();
    if (p.empty()) return false;
    std::ifstream f(p);
    if (!f) return false;
    std::string line;
    int rx = -1, ry = -1, rw = -1, rh = -1;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if      (key == "x") rx = std::atoi(val.c_str());
        else if (key == "y") ry = std::atoi(val.c_str());
        else if (key == "w") rw = std::atoi(val.c_str());
        else if (key == "h") rh = std::atoi(val.c_str());
    }
    if (rx < 0 || ry < 0 || rw < 0 || rh < 0) return false;
    x = rx; y = ry; w = rw; h = rh;
    return true;
}

static void saveWindowState(int x, int y, int w, int h) {
    std::string p = getStateFilePath();
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (!f) return;
    f << "x=" << x << "\n";
    f << "y=" << y << "\n";
    f << "w=" << w << "\n";
    f << "h=" << h << "\n";
}
// 取得文件 mtime（秒级精度），失败返 0
static time_t getFileMtime(const std::string& p) {
    struct _stat st;
    if (_stat(p.c_str(), &st) != 0) return 0;
    return st.st_mtime;
}

// ---------------- UDP 控制 ----------------
static SOCKET g_udp = INVALID_SOCKET;

static bool udpInit(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(g_udp, FIONBIO, &nb);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);
    if (bind(g_udp, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_udp);
        g_udp = INVALID_SOCKET;
        return false;
    }
    return true;
}

// 非阻塞收一条命令；收到返回 true（保留原始字节，可能含 \0——OSC 是二进制协议）
static bool udpPoll(std::string& cmd, sockaddr_in& from) {
    if (g_udp == INVALID_SOCKET) return false;
    char buf[512];
    int fromLen = sizeof(from);
    int n = recvfrom(g_udp, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n <= 0) return false;
    cmd.assign(buf, n);
    return true;
}

static void udpReply(const sockaddr_in& to, const std::string& msg) {
    if (g_udp == INVALID_SOCKET) return;
    sendto(g_udp, msg.c_str(), (int)msg.size(), 0, (const sockaddr*)&to, sizeof(to));
}

// ---------------- OSC（极简 OSC 1.0 解析，与文本命令共用 UDP 端口） ----------------
// 支持：/play /pause /toggle /restart /next /prev /quit /mute（无参）
//       /queue <int>  /volume <float 0~1>；支持 #bundle 简单展开
static int oscReadPaddedString(const char* buf, int len, int pos, std::string& out) {
    if (pos >= len) return -1;
    int slen = 0;
    while (pos + slen < len && buf[pos + slen] != '\0') slen++;
    if (pos + slen >= len) return -1;             // 没有 \0 结尾
    out.assign(buf + pos, slen);
    return pos + ((slen + 4) & ~3);               // 含 \0，4 字节对齐
}

static bool oscParseMessage(const char* buf, int len, std::string& addr,
                            double& arg0, bool& hasArg) {
    hasArg = false; arg0 = 0.0;
    int pos = oscReadPaddedString(buf, len, 0, addr);
    if (pos < 0 || addr.empty() || addr[0] != '/') return false;
    if (pos >= len) return true;                  // 无参数
    std::string tags;
    pos = oscReadPaddedString(buf, len, pos, tags);
    if (pos < 0 || tags.empty() || tags[0] != ',') return true;
    if (tags.size() >= 2 && pos + 4 <= len) {
        const uint8_t* p = (const uint8_t*)buf + pos;
        uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                   | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        if (tags[1] == 'i')      { arg0 = (double)(int32_t)u; hasArg = true; }
        else if (tags[1] == 'f') { float f; memcpy(&f, &u, 4); arg0 = f; hasArg = true; }
    }
    return true;
}

// 解析一个 UDP 包（可能是 OSC message / bundle / 纯文本），产出文本命令
// 返回 false = 无法识别
static bool oscPacketToCommand(const std::string& pkt, std::string& cmdOut) {
    if (pkt.empty() || (pkt[0] != '/' && pkt[0] != '#')) {
        cmdOut = pkt;   // 纯文本命令，原样
        return true;
    }
    if (pkt[0] == '/') {
        std::string addr; double arg; bool hasArg;
        if (!oscParseMessage(pkt.data(), (int)pkt.size(), addr, arg, hasArg)) return false;
        cmdOut = addr.substr(1);
        if (hasArg) {
            char tmp[48];
            std::snprintf(tmp, sizeof(tmp), " %g", arg);
            cmdOut += tmp;
        }
        return true;
    }
    // #bundle\0 + 8 字节 timetag + [int32 size + message] × N（只取第一个有效消息）
    if (pkt.compare(0, 7, "#bundle") == 0 && pkt.size() >= 20) {
        int pos = 16;
        while (pos + 4 <= (int)pkt.size()) {
            const uint8_t* p = (const uint8_t*)pkt.data() + pos;
            int mlen = (int)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                           | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
            pos += 4;
            if (mlen <= 0 || pos + mlen > (int)pkt.size()) break;
            std::string addr; double arg; bool hasArg;
            if (oscParseMessage(pkt.data() + pos, mlen, addr, arg, hasArg)) {
                cmdOut = addr.empty() ? "" : addr.substr(1);
                if (hasArg) { char tmp[48]; std::snprintf(tmp, sizeof(tmp), " %g", arg); cmdOut += tmp; }
                return true;   // 只执行第一个有效消息
            }
            pos += mlen;
        }
    }
    return false;
}

static void keyCallback(GLFWwindow*, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(g_win, GLFW_TRUE);
    else if (key == GLFW_KEY_SPACE) g_paused = !g_paused;
    else if (key == GLFW_KEY_R) g_restart = true;
    else if (key == GLFW_KEY_F) {
        g_fullscreen = !g_fullscreen;
        if (g_fullscreen) {
            glfwGetWindowPos(g_win, &g_winX, &g_winY);
            glfwGetWindowSize(g_win, &g_winW, &g_winH);
            GLFWmonitor* mon = g_targetMonitor ? g_targetMonitor : glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(mon);
            glfwSetWindowMonitor(g_win, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            glfwSetWindowMonitor(g_win, nullptr, g_winX, g_winY, g_winW, g_winH, 0);
        }
    }
    else if (key == GLFW_KEY_M) {
        // M 键 = 静音切换（仅在音频已初始化时生效）
        // 注：实现依赖 audio.cpp 的内部 SDL_AudioStream；用 helper 函数切换
        audio::Context::instance().toggleMute();
    }
}

// 音频初始化（多源混音：所有带音频流的层 + 可选外挂 [audio] source）
// 内嵌源跟随层的 playing 状态发声，音量取图层行第 7 字段 |音量%
static void initAudioFromConfig(const std::vector<Queue>& queues) {
    std::vector<audio::SourceSpec> specs;
    for (const auto& q : queues)
        for (const auto& L : q.layers)
            if (L.audioStreamIdx >= 0) {
                audio::SourceSpec sp;
                sp.path = L.path;
                sp.streamIdx = L.audioStreamIdx;
                sp.volume = L.volume;
                sp.loop = L.loop;
                sp.alwaysOn = false;   // 跟随层状态
                specs.push_back(sp);
            }
    if (!g_audioCfg.source.empty()) {
        audio::SourceSpec sp;
        sp.path = g_audioCfg.source;
        sp.streamIdx = -1;
        sp.volume = g_audioCfg.volume;
        sp.loop = g_audioCfg.loop;
        sp.alwaysOn = true;            // 外挂音频不受层状态控制
        specs.push_back(sp);
    }
    audio::Context::instance().initSources(specs, glfwGetTime());
}

// 建纹理（GL 上下文就绪后调用）
static void initLayerTexture(Layer& L) {
    glGenTextures(1, &L.tex);
    glBindTexture(GL_TEXTURE_2D, L.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// B-12 真热重载：重新解析 layers.txt 并整体重建媒体层
// 队列/图层/音频/scale 立即生效；窗口几何（size/pos/monitor/fullscreen/borderless）仍需重启进程
// 新配置无可用队列时忽略本次变更、继续播旧配置（现场改配置打错字不会黑屏）
static bool reloadMedia(std::vector<Queue>& queues, const std::string& configPath,
                        WindowCfg& win, size_t& qi) {
    std::cout << "[热重载] 重新解析配置...\n";
    WindowCfg fresh;
    std::vector<Queue> newQueues = loadConfig(configPath, fresh);
    if (newQueues.empty()) {
        std::cerr << "[热重载] 新配置无可用队列，忽略本次变更，继续播旧配置\n";
        return false;
    }

    audio::Context::instance().shutdown();
    for (auto& q : queues)
        for (auto& L : q.layers) closeLayer(L);
    queues = std::move(newQueues);
    win.scale = fresh.scale;   // 媒体相关配置热生效；窗口几何保持现状
    win.fade = fresh.fade;

    for (auto& q : queues)
        for (auto& L : q.layers) initLayerTexture(L);
    if (g_preloadEnabled)
        for (auto& q : queues)
            for (auto& L : q.layers) preloadLayer(L);
    for (auto& q : queues)
        for (auto& L : q.layers) startDecoder(L);
    initAudioFromConfig(queues);

    qi = 0;
    for (auto& L : queues[0].layers) resetLayer(L);
    preRoll(queues[0], 5.0);
    std::cout << "[热重载] 完成，从队列 1 开始\n";
    return true;
}

// 场间淡入淡出状态机（g_fadeAlpha 在 drawLayer 上方定义）
static int    g_fadeState = 0;      // 0=无 1=淡出中 2=淡入中
static double g_fadeT0 = 0.0;
static int    g_switchTarget = -1;  // >=0：请求切到该队列（淡出完成后执行）

// ---------------- HAP Q 颜色日志验证（无需肉眼） ----------------
// 用法: happlayer --verify-hapq <file.mov> <帧号> <参考帧.rgba>
// 参考帧生成: ffmpeg -i file.mov -vf "select=eq(n\,N)" -vframes 1 -f rawvideo -pix_fmt rgba ref.rgba
// 流程：我们的 hap::decodeInto 解码 → GPU DXT5 采样 + HAP Q shader → glReadPixels 读回
//       → 与 ffmpeg CPU 解码的参考帧逐像素对比（同时验证颜色公式与垂直方向）
static int verifyHapQ(const std::string& path, int wantFrame, const std::string& refPath) {
    // 1) 解封装 + 我们的解码器解出第 wantFrame 帧 DXT
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[verify] 打不开 " << path << "\n";
        return 2;
    }
    avformat_find_stream_info(fmt, nullptr);
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) { std::cerr << "[verify] 没有视频流\n"; return 2; }
    int W = fmt->streams[vidx]->codecpar->width;
    int H = fmt->streams[vidx]->codecpar->height;

    std::vector<uint8_t> dxt, dxt2;
    hap::PixFmt pfmt = hap::PixFmt::DXT5;
    std::string err;
    bool got = false;
    AVPacket* pkt = av_packet_alloc();
    for (int i = 0; i <= wantFrame && !got;) {
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
        if (i == wantFrame) {
            got = hap::decodeInto(pkt->data, pkt->size, dxt, pfmt, err, &dxt2);
        }
        i++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    if (!got) { std::cerr << "[verify] 解码第 " << wantFrame << " 帧失败: " << err << "\n"; return 2; }
    if (pfmt != hap::PixFmt::YCoCg_DXT5) {
        std::cerr << "[verify] 该文件不是 HAP Q（YCoCg），无需本验证\n";
        return 2;
    }

    // 2) 读参考帧（ffmpeg CPU 解码，rgba）
    FILE* f = fopen(refPath.c_str(), "rb");
    if (!f) { std::cerr << "[verify] 打不开参考帧 " << refPath << "\n"; return 2; }
    std::vector<uint8_t> ref((size_t)W * H * 4);
    bool refOk = fread(ref.data(), 1, ref.size(), f) == ref.size();
    fclose(f);
    if (!refOk) { std::cerr << "[verify] 参考帧尺寸不符（应为 " << W << "x" << H << " rgba）\n"; return 2; }

    // 3) GL 渲染一帧并读回
    if (!glfwInit()) { std::cerr << "[verify] glfwInit 失败\n"; return 2; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(W, H, "verify", nullptr, nullptr);
    if (!w) { std::cerr << "[verify] 创建窗口失败\n"; glfwTerminate(); return 2; }
    glfwMakeContextCurrent(w);

    pglCompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)glfwGetProcAddress("glCompressedTexImage2D");
    pglCreateShader        = (PFNGLCREATESHADERPROC)      glfwGetProcAddress("glCreateShader");
    pglShaderSource        = (PFNGLSHADERSOURCEPROC)      glfwGetProcAddress("glShaderSource");
    pglCompileShader       = (PFNGLCOMPILESHADERPROC)     glfwGetProcAddress("glCompileShader");
    pglGetShaderiv         = (PFNGLGETSHADERIVPROC)       glfwGetProcAddress("glGetShaderiv");
    pglGetShaderInfoLog    = (PFNGLGETSHADERINFOLOGPROC)  glfwGetProcAddress("glGetShaderInfoLog");
    pglDeleteShader        = (PFNGLDELETESHADERPROC)      glfwGetProcAddress("glDeleteShader");
    pglCreateProgram       = (PFNGLCREATEPROGRAMPROC)     glfwGetProcAddress("glCreateProgram");
    pglAttachShader        = (PFNGLATTACHSHADERPROC)      glfwGetProcAddress("glAttachShader");
    pglLinkProgram         = (PFNGLLINKPROGRAMPROC)       glfwGetProcAddress("glLinkProgram");
    pglGetProgramiv        = (PFNGLGETPROGRAMIVPROC)      glfwGetProcAddress("glGetProgramiv");
    pglGetProgramInfoLog   = (PFNGLGETPROGRAMINFOLOGPROC) glfwGetProcAddress("glGetProgramInfoLog");
    pglDeleteProgram       = (PFNGLDELETEPROGRAMPROC)     glfwGetProcAddress("glDeleteProgram");
    pglUseProgram          = (PFNGLUSEPROGRAMPROC)        glfwGetProcAddress("glUseProgram");
    pglGetUniformLocation  = (PFNGLGETUNIFORMLOCATIONPROC)glfwGetProcAddress("glGetUniformLocation");
    pglUniform1i           = (PFNGLUNIFORM1IPROC)         glfwGetProcAddress("glUniform1i");
    pglUniform1f           = (PFNGLUNIFORM1FPROC)         glfwGetProcAddress("glUniform1f");
    pglGetAttribLocation   = (PFNGLGETATTRIBLOCATIONPROC) glfwGetProcAddress("glGetAttribLocation");
    pglVertexAttrib2f      = (PFNGLVERTEXATTRIB2FPROC)    glfwGetProcAddress("glVertexAttrib2f");
    pglBindAttribLocation  = (PFNGLBINDATTRIBLOCATIONPROC)glfwGetProcAddress("glBindAttribLocation");
    pglActiveTexture       = (PFNGLACTIVETEXTUREPROC)     glfwGetProcAddress("glActiveTexture");
    if (!pglCompressedTexImage2D || !pglCreateShader) {
        std::cerr << "[verify] GL 函数加载失败\n";
        return 2;
    }
    initShaders();
    if (!g_hapQProgram) { std::cerr << "[verify] HAP Q shader 编译失败\n"; return 2; }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pglCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                            W, H, 0, (GLsizei)dxt.size(), dxt.data());

    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    pglUseProgram(g_hapQProgram);
    pglUniform1i(g_hapQUTex, 0);
    if (g_hapQUAlpha >= 0) pglUniform1f(g_hapQUAlpha, 1.0f);
    // 与 drawLayer 一致的方向：UV(0,0)=视频顶 → NDC +1；先写 UV 再写 Pos（location 0 触发发射）
    glBegin(GL_QUADS);
    pglVertexAttrib2f(g_hapQAttrUV, 0.f, 0.f); pglVertexAttrib2f(g_hapQAttrPos, -1,  1);
    pglVertexAttrib2f(g_hapQAttrUV, 1.f, 0.f); pglVertexAttrib2f(g_hapQAttrPos,  1,  1);
    pglVertexAttrib2f(g_hapQAttrUV, 1.f, 1.f); pglVertexAttrib2f(g_hapQAttrPos,  1, -1);
    pglVertexAttrib2f(g_hapQAttrUV, 0.f, 1.f); pglVertexAttrib2f(g_hapQAttrPos, -1, -1);
    glEnd();
    pglUseProgram(0);

    std::vector<uint8_t> out((size_t)W * H * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());   // 行 0 = 画面底部

    // 4) 逐像素对比（读回帧垂直翻转到顶向下；只比 RGB，跳过 A）
    //    附带 ±1px 平移搜索：区分"公式错"与"映射偏移/翻转"
    auto diffAt = [&](int dy, int dx, double& meanOut, int& maxOut, double& badPctOut) {
        double sum = 0; int mx = 0; size_t bad = 0; size_t cnt = 0;
        for (int y = 2; y < H - 2; ++y) {
            int sy = y + dy; if (sy < 0 || sy >= H) continue;
            const uint8_t* rowOut = out.data() + (size_t)(H - 1 - sy) * W * 4;  // 翻转
            const uint8_t* rowRef = ref.data() + (size_t)y * W * 4;
            for (int x = 2; x < W - 2; ++x) {
                int sx = x + dx; if (sx < 0 || sx >= W) continue;
                int pd = 0;
                for (int c = 0; c < 3; ++c) {
                    int d = std::abs((int)rowOut[sx * 4 + c] - (int)rowRef[x * 4 + c]);
                    if (d > pd) pd = d;
                }
                sum += pd; cnt++;
                if (pd > mx) mx = pd;
                if (pd > 8) bad++;
            }
        }
        meanOut = cnt ? sum / cnt : 1e9;
        maxOut = mx;
        badPctOut = cnt ? (double)bad / cnt * 100.0 : 100.0;
    };
    double meanDiff; int maxDiff; double badRatio;
    diffAt(0, 0, meanDiff, maxDiff, badRatio);
    std::cout << "[verify] 帧 " << wantFrame << "  " << W << "x" << H
              << "  平均差=" << meanDiff << "  最大差=" << maxDiff
              << "  超阈值像素=" << badRatio << "%\n";
    // 平移/翻转诊断
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dy && !dx) continue;
            double m; int mx2; double b;
            diffAt(dy, dx, m, mx2, b);
            std::cout << "[verify]   平移(" << dy << "," << dx << "): 平均差=" << m << "\n";
        }
    // 采样点打印（左上角 4 像素 + 中心）
    for (int k = 0; k < 4; ++k) {
        int y = 0, x = k;
        const uint8_t* po = out.data() + (size_t)(H - 1 - y) * W * 4 + x * 4;
        const uint8_t* pr = ref.data() + (size_t)y * W * 4 + x * 4;
        std::cout << "[verify]   像素(0," << k << ") 我们=("
                  << (int)po[0] << "," << (int)po[1] << "," << (int)po[2] << ") 参考=("
                  << (int)pr[0] << "," << (int)pr[1] << "," << (int)pr[2] << ")\n";
    }
    {
        int y = H / 2, x = W / 2;
        const uint8_t* po = out.data() + (size_t)(H - 1 - y) * W * 4 + x * 4;
        const uint8_t* pr = ref.data() + (size_t)y * W * 4 + x * 4;
        std::cout << "[verify]   中心像素 我们=("
                  << (int)po[0] << "," << (int)po[1] << "," << (int)po[2] << ") 参考=("
                  << (int)pr[0] << "," << (int)pr[1] << "," << (int)pr[2] << ")\n";
    }
    bool pass = meanDiff < 2.0 && maxDiff <= 24 && badRatio < 0.5;
    std::cout << "[verify] HAP Q 颜色/方向验证: " << (pass ? "PASS ✓" : "FAIL ✗") << "\n";

    glDeleteTextures(1, &tex);
    glfwDestroyWindow(w);
    glfwTerminate();
    return pass ? 0 : 1;
}

int main(int argc, char** argv) {
    // Windows 默认计时器粒度 15.6ms，sleep_for(500us) 实际睡 ~16ms，frame pacing 直接翻车
    // （实测 fps 被拖到 32）。提到 1ms 后 pacing 才能命中 60fps。
    timeBeginPeriod(1);

    // 日志验证模式：HAP Q 颜色/方向逐像素对比（免肉眼）
    if (argc >= 5 && std::string(argv[1]) == "--verify-hapq")
        return verifyHapQ(argv[2], std::atoi(argv[3]), argv[4]);

    // 开机自启三件套 — 防休眠：
    // ES_CONTINUOUS   — 持续生效（默认 ON 状态，进程退出需显式 ES_CONTINUOUS 还原）
    // ES_SYSTEM_REQUIRED  — 阻止系统进入睡眠
    // ES_DISPLAY_REQUIRED — 阻止显示器关闭
    // 配 install-autostart.bat 使用，展项无人值守播放时不会黑屏/休眠
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    std::string configPath = "layers.txt";
    double benchSeconds = 0.0;
    int udpPort = 7000;
    // 命令行覆盖项（-1 = 未指定，用配置文件的值）
    int ovW = -1, ovH = -1, ovX = -1, ovY = -1, ovMon = -1, ovFs = -1, ovBl = -1, ovScale = -1;
    double ovFade = -1.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bench" && i + 1 < argc) benchSeconds = std::atof(argv[++i]);
        else if (a == "--port" && i + 1 < argc) udpPort = std::atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &ovW, &ovH) != 2) {
                std::cerr << "--size 格式应为 宽x高\n";
                return 1;
            }
        }
        else if (a == "--pos" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d%*[,xX]%d", &ovX, &ovY) != 2) {
                std::cerr << "--pos 格式应为 x,y\n";
                return 1;
            }
        }
        else if (a == "--monitor" && i + 1 < argc) ovMon = std::atoi(argv[++i]);
        else if (a == "--scale" && i + 1 < argc) ovScale = std::atoi(argv[++i]);
        else if (a == "--fade" && i + 1 < argc) ovFade = std::atof(argv[++i]);
        else if (a == "--fullscreen") ovFs = 1;
        else if (a == "--borderless") ovBl = 1;
        else if (a == "--max-fps" && i + 1 < argc) g_maxFps = std::atof(argv[++i]);
        // 小项 1：JSON bench 报告开关
        else if (a == "--json") g_jsonBench = true;
        // 小项 2：日志级别（error/warn/info/debug）
        else if (a == "--log-level" && i + 1 < argc) {
            std::string lvl = argv[++i];
            if      (lvl == "error") g_logLevel = 0;
            else if (lvl == "warn")  g_logLevel = 1;
            else if (lvl == "info")  g_logLevel = 2;
            else if (lvl == "debug") g_logLevel = 3;
            else { std::cerr << "--log-level 取值应为 error|warn|info|debug（默认 info）\n"; return 1; }
        }
        else configPath = a;
    }

    WindowCfg win;
    std::vector<Queue> queues = loadConfig(configPath, win);
    if (queues.empty()) {
        std::cerr << "没有可用队列，请检查 " << configPath << "\n";
        return 1;
    }
    // 配置热重载：记录 layers.txt 的初始 mtime
    g_configMtime = getFileMtime(configPath);
    std::cout << "[热重载] 监听 " << configPath << "（mtime=" << g_configMtime << "）\n";
    // 资源预加载：对 < max_duration 秒的非 HAP 层一次性解到内存（消除冷启动卡顿）
    // 注意：必须在所有层 openLayer 之后、startDecoder 之前调用
    //      preloadLayer 自己 seek 到 0，再次激活需要 resetLayer 重置 gen
    if (g_preloadEnabled) {
        std::cout << "[preload] max_duration=" << g_preloadMaxDuration << "s\n";
        for (auto& q : queues) {
            for (auto& L : q.layers) preloadLayer(L);
        }
    } else {
        std::cout << "[preload] 配置关闭，跳过\n";
    }
    // 命令行覆盖配置文件
    if (ovW > 0)  win.w = ovW;
    if (ovH > 0)  win.h = ovH;
    if (ovX >= 0) win.x = ovX;
    if (ovY >= 0) win.y = ovY;
    if (ovMon > 0) win.monitor = ovMon;
    if (ovScale > 0) win.scale = ovScale;
    if (ovFade >= 0) win.fade = ovFade;
    if (ovFs >= 0) win.fullscreen = ovFs != 0;
    if (ovBl >= 0) win.borderless = ovBl != 0;

    if (!glfwInit()) { std::cerr << "glfwInit 失败\n"; return 1; }

    // 音频初始化（优先级：HAP 自带音频 > 配置 [audio] source > 无音频）
    initAudioFromConfig(queues);

    // 枚举显示器
    int mcount = 0;
    GLFWmonitor** mons = glfwGetMonitors(&mcount);
    std::cout << "检测到 " << mcount << " 台显示器：\n";
    for (int i = 0; i < mcount; ++i) {
        int mx, my;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
        std::cout << "  " << (i + 1) << ": " << vm->width << "x" << vm->height
                  << " @" << vm->refreshRate << "Hz  虚拟坐标(" << mx << "," << my << ")"
                  << (mons[i] == glfwGetPrimaryMonitor() ? "  [主]" : "") << "\n";
    }
    int mi = win.monitor - 1;
    if (mi < 0 || mi >= mcount) {
        std::cerr << "[警告] 显示器序号 " << win.monitor << " 超出范围，用 1\n";
        mi = 0;
    }
    GLFWmonitor* target = mons[mi];
    g_targetMonitor = target;
    int tmx, tmy;
    glfwGetMonitorPos(target, &tmx, &tmy);

    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    if (g_useVulkan) {
        // Vulkan 不能和 OpenGL context 共用同一个 HWND，否则 vkCreateWin32Surface 会 Access Violation
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    // 小项 3：启动时尝试从 %APPDATA%/happlayer/state.json 读取上次窗口位置
    // 命令行 / 配置覆盖过的尺寸优先（不覆盖）
    int savedX = 0, savedY = 0, savedW = 0, savedH = 0;
    bool hasSaved = loadWindowState(savedX, savedY, savedW, savedH);
    if (hasSaved) {
        LOG(2, "[窗口] 从 state.json 恢复上次位置 (" << savedX << "," << savedY
                  << ") " << savedW << "x" << savedH << "\n");
    }
    if (win.fullscreen) {
        const GLFWvidmode* vm = glfwGetVideoMode(target);
        g_win = glfwCreateWindow(vm->width, vm->height, "happlayer", target, nullptr);
        std::cout << "窗口: 全屏 @ 显示器 " << (mi + 1)
                  << "（" << vm->width << "x" << vm->height << "）\n";
    } else {
        glfwWindowHint(GLFW_DECORATED, win.borderless ? GLFW_FALSE : GLFW_TRUE);
        g_win = glfwCreateWindow(win.w, win.h, "happlayer", nullptr, nullptr);
        if (g_win) {
            // 优先用命令行/配置文件的位置；配置文件未指定 + 有保存值 → 用 saved
            int px = (ovX >= 0 || win.x != 0) ? (tmx + win.x) : (savedX > 0 ? savedX : tmx + win.x);
            int py = (ovY >= 0 || win.y != 0) ? (tmy + win.y) : (savedY > 0 ? savedY : tmy + win.y);
            glfwSetWindowPos(g_win, px, py);
            // 记录本次窗口尺寸（用于退出时保存）
            g_winX = px; g_winY = py;
            glfwGetWindowSize(g_win, &g_winW, &g_winH);
        }
        std::cout << "窗口: " << win.w << "x" << win.h
                  << " @ 显示器 " << (mi + 1) << " 偏移(" << win.x << "," << win.y << ")"
                  << (win.borderless ? " 无边框" : "")
                  << (win.scale != 100 ? (" 缩放" + std::to_string(win.scale) + "%") : "") << "\n";
    }
    if (!g_win) { std::cerr << "创建窗口失败\n"; glfwTerminate(); return 1; }

    // Win11 下所有窗口（包括全屏）都会被打上圆角，强制直角（与 borderless 无关）
    // 日志输出设置结果 + 回读确认，便于无肉眼核对
    {
        HWND hwnd = glfwGetWin32Window(g_win);
        DWORD pref = DWMWCP_DONOTROUND;
        HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        DWORD actual = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &actual, sizeof(actual));
        std::cout << "[窗口] 圆角抑制: DwmSet=" << (SUCCEEDED(hr) ? "OK" : "失败")
                  << " 回读策略=" << actual << "（1=DONOTROUND 直角）\n";
    }

    glfwSetKeyCallback(g_win, keyCallback);
    if (!g_useVulkan) {
        glfwMakeContextCurrent(g_win);
        glfwSwapInterval(0); // 关闭垂直同步（启用自定义 frame pacing，更稳）
    }

#ifdef HAPPLAYER_HAS_VULKAN
    // 设置环境变量 HAPPLAYER_USE_VULKAN=1 走 Vulkan 路径（默认仍 OpenGL）
    if (g_useVulkan) {
        HWND vkHwnd = glfwGetWin32Window(g_win);
        int vkW = 0, vkH = 0;
        glfwGetFramebufferSize(g_win, &vkW, &vkH);
        if (!vk_render::Renderer::instance().init(vkHwnd, vkW, vkH)) {
            std::cerr << "[vulkan] init 失败（GLFW_NO_API 窗口无法回退 OpenGL，退出）\n";
            glfwDestroyWindow(g_win);
            glfwTerminate();
            return 1;
        }
        std::cout << "[vulkan] 多层渲染管线已启用（阶段 E，逐帧 DXT/RGBA 上传）\n";
    }
#endif

    const bool vkReady =
#ifdef HAPPLAYER_HAS_VULKAN
        (g_useVulkan && vk_render::Renderer::instance().isInitialized());
#else
        false;
#endif
    if (!vkReady) {
    pglCompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)glfwGetProcAddress("glCompressedTexImage2D");
    pglCompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)glfwGetProcAddress("glCompressedTexSubImage2D");
    if (!pglCompressedTexImage2D || !pglCompressedTexSubImage2D) {
        std::cerr << "显卡驱动不支持 S3TC 压缩纹理\n";
        return 1;
    }
    // P0.1 PBO + Fence 异步上传所需函数（OpenGL 1.5/3.2 扩展）
    pglGenBuffers        = (PFNGLGENBUFFERSPROC)      glfwGetProcAddress("glGenBuffers");
    pglDeleteBuffers     = (PFNGLDELETEBUFFERSPROC)   glfwGetProcAddress("glDeleteBuffers");
    pglBindBuffer        = (PFNGLBINDBUFFERPROC)      glfwGetProcAddress("glBindBuffer");
    pglBufferData        = (PFNGLBUFFERDATAPROC)      glfwGetProcAddress("glBufferData");
    pglMapBufferRange    = (PFNGLMAPBUFFERRANGEPROC)  glfwGetProcAddress("glMapBufferRange");
    pglUnmapBuffer       = (PFNGLUNMAPBUFFERPROC)     glfwGetProcAddress("glUnmapBuffer");
    pglFenceSync         = (PFNGLFENCESYNCPROC)       glfwGetProcAddress("glFenceSync");
    pglDeleteSync        = (PFNGLDELETESYNCPROC)      glfwGetProcAddress("glDeleteSync");
    pglClientWaitSync    = (PFNGLCLIENTWAITSYNCPROC)  glfwGetProcAddress("glClientWaitSync");
    g_pboAvailable = pglGenBuffers && pglDeleteBuffers && pglBindBuffer && pglBufferData
                  && pglMapBufferRange && pglUnmapBuffer && pglFenceSync && pglDeleteSync
                  && pglClientWaitSync
                  && !std::getenv("HAPPLAYER_NO_PBO");   // 排障开关：怀疑 fence 慢时强制同步路径
    if (!g_pboAvailable) {
        std::cerr << "[警告] PBO/Fence 不可用（驱动不支持或 HAPPLAYER_NO_PBO=1），纹理上传走同步路径\n";
    }
    // P1.1 HAP Q shader 函数加载
    pglCreateShader        = (PFNGLCREATESHADERPROC)      glfwGetProcAddress("glCreateShader");
    pglShaderSource        = (PFNGLSHADERSOURCEPROC)      glfwGetProcAddress("glShaderSource");
    pglCompileShader       = (PFNGLCOMPILESHADERPROC)     glfwGetProcAddress("glCompileShader");
    pglGetShaderiv         = (PFNGLGETSHADERIVPROC)       glfwGetProcAddress("glGetShaderiv");
    pglGetShaderInfoLog    = (PFNGLGETSHADERINFOLOGPROC)  glfwGetProcAddress("glGetShaderInfoLog");
    pglDeleteShader        = (PFNGLDELETESHADERPROC)      glfwGetProcAddress("glDeleteShader");
    pglCreateProgram       = (PFNGLCREATEPROGRAMPROC)     glfwGetProcAddress("glCreateProgram");
    pglAttachShader        = (PFNGLATTACHSHADERPROC)      glfwGetProcAddress("glAttachShader");
    pglLinkProgram         = (PFNGLLINKPROGRAMPROC)       glfwGetProcAddress("glLinkProgram");
    pglGetProgramiv        = (PFNGLGETPROGRAMIVPROC)      glfwGetProcAddress("glGetProgramiv");
    pglGetProgramInfoLog   = (PFNGLGETPROGRAMINFOLOGPROC) glfwGetProcAddress("glGetProgramInfoLog");
    pglDeleteProgram       = (PFNGLDELETEPROGRAMPROC)     glfwGetProcAddress("glDeleteProgram");
    pglUseProgram          = (PFNGLUSEPROGRAMPROC)        glfwGetProcAddress("glUseProgram");
    pglGetUniformLocation  = (PFNGLGETUNIFORMLOCATIONPROC)glfwGetProcAddress("glGetUniformLocation");
    pglUniform1i           = (PFNGLUNIFORM1IPROC)         glfwGetProcAddress("glUniform1i");
    pglUniform1f           = (PFNGLUNIFORM1FPROC)         glfwGetProcAddress("glUniform1f");
    pglGetAttribLocation   = (PFNGLGETATTRIBLOCATIONPROC) glfwGetProcAddress("glGetAttribLocation");
    pglVertexAttrib2f      = (PFNGLVERTEXATTRIB2FPROC)    glfwGetProcAddress("glVertexAttrib2f");
    pglBindAttribLocation  = (PFNGLBINDATTRIBLOCATIONPROC)glfwGetProcAddress("glBindAttribLocation");
    pglActiveTexture       = (PFNGLACTIVETEXTUREPROC)     glfwGetProcAddress("glActiveTexture");
    if (!pglCreateShader || !pglUseProgram) {
        std::cerr << "[警告] 显卡驱动不支持 GLSL，P1.1 HAP Q shader 将自动降级为无 shader（直接画 RGB）\n";
    }

    // P1.1 编译 HAP Q 着色器（失败不致命，没有 HAP Q 素材时用不到）
    initHapQShader();
    }

    // 全部图层：建纹理 + 启动解码线程（未激活的线程挂起等待）
    // Vulkan 路径不建 OpenGL 纹理（避免双份显存）
    for (auto& q : queues)
        for (auto& L : q.layers) {
            if (!vkReady) {
                glGenTextures(1, &L.tex);
                glBindTexture(GL_TEXTURE_2D, L.tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            startDecoder(L);
        }

    if (!vkReady) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0, 0, 0, 1);
    }

    std::cout << "开始播放。空格=暂停  R=重播当前队列  F=全屏  Esc=退出\n";
    if (udpPort > 0) {
        if (udpInit(udpPort))
            std::cout << "UDP 控制端口 " << udpPort
                      << "：play/pause/toggle/restart/next/prev/queue N/status/quit\n";
        else
            std::cerr << "[警告] UDP 端口 " << udpPort << " 绑定失败，控制接口不可用\n";
    }

    size_t qi = 0;
    for (auto& L : queues[0].layers) resetLayer(L);
    preRoll(queues[0], 5.0);
    std::cout << ">>> 队列 1/" << queues.size() << " 开始\n";

    double q0 = glfwGetTime();   // 当前队列的起始时刻
    double benchStart = q0;      // bench 计时起点（墙钟）
    double pauseStart = 0.0, pausedTotal = 0.0;
    int frames = 0;
    double fpsWinStart = q0, statLine = q0;
    double fps = 0.0;

    // 切场执行体：停旧队列解码 → 新队列复位预读 → 时钟归零 → 音频对齐
    auto doQueueSwitch = [&](size_t target) {
        for (auto& L : queues[qi].layers) deactivateLayer(L);
        qi = target;
        for (auto& L : queues[qi].layers) resetLayer(L);
        preRoll(queues[qi], 5.0);
        q0 = glfwGetTime(); pausedTotal = 0.0; pauseStart = 0.0; g_paused = false;
        audio::Context::instance().onQueueStart(q0);
        std::cout << ">>> 队列 " << (qi + 1) << "/" << queues.size() << " 开始\n";
    };

    while (!glfwWindowShouldClose(g_win)) {
        double now = glfwGetTime();

        // ---- 音频同步（每帧调一次）----
        // tick(queueClock, paused)：用视频时钟校正音频；pause 时只暂停 SDL 设备
        audio::Context::instance().tick(now - q0 - pausedTotal, g_paused);

        // ---- UDP 命令处理（每帧抽空 socket） ----
        {
            std::string cmd;
            sockaddr_in from{};
            while (udpPoll(cmd, from)) {
                // OSC（/ 或 # 开头）→ 转文本命令；纯文本原样
                std::string text;
                if (!oscPacketToCommand(cmd, text)) {
                    udpReply(from, "ERR bad packet");
                    continue;
                }
                // 去首尾空白、转小写
                std::string c = trim(text);
                for (auto& ch : c) ch = (char)tolower((unsigned char)ch);
                std::cout << ((!cmd.empty() && (cmd[0] == '/' || cmd[0] == '#')) ? "<<< OSC: " : "<<< UDP: ")
                          << c << "\n";
                std::string reply = "OK " + c;
                if (c == "play") g_paused = false;
                else if (c == "pause") g_paused = true;
                else if (c == "toggle") g_paused = !g_paused;
                else if (c == "restart") g_restart = true;
                else if (c == "next") g_queueStep = 1;
                else if (c == "prev") g_queueStep = -1;
                else if (c.rfind("queue", 0) == 0) {
                    int n = std::atoi(c.c_str() + 5);
                    if (n >= 1 && n <= (int)queues.size()) g_queueJump = n - 1;
                    else reply = "ERR queue index out of range";
                }
                else if (c == "status") {
                    std::ostringstream ss;
                    double tNow = g_paused ? (pauseStart - q0 - pausedTotal)
                                           : (now - q0 - pausedTotal);
                    ss << "STATUS queue=" << (qi + 1) << "/" << queues.size()
                       << " t=" << (long)(tNow * 10) / 10.0
                       << " paused=" << (g_paused ? 1 : 0) << " fps=" << (long)(fps + 0.5);
                    for (const auto& L : queues[qi].layers)
                        ss << " | " << L.name << "=" << L.state;
                    reply = ss.str();
                }
                else if (c == "mute") audio::Context::instance().toggleMute();
                else if (c.rfind("volume", 0) == 0) {
                    float v = (float)std::atof(c.c_str() + 6);
                    if (v < 0.0f) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    audio::Context::instance().setMasterVolume(v);
                }
                else if (c == "quit") glfwSetWindowShouldClose(g_win, GLFW_TRUE);
                else reply = "ERR unknown command: " + c;
                udpReply(from, reply);
            }
        }

        if (g_restart) {   // 重播当前队列
            g_restart = false;
            for (auto& L : queues[qi].layers) resetLayer(L);
            preRoll(queues[qi], 5.0);
            q0 = glfwGetTime(); pausedTotal = 0.0; g_paused = false; pauseStart = 0.0;
            now = q0;
            audio::Context::instance().onQueueStart(q0);   // 音频：seek 到 0 + 重置时钟
            std::cout << ">>> 队列 " << (qi + 1) << " 重播\n";
        }
        // 手动切场 → 走淡入淡出状态机
        if (g_queueStep != 0 || g_queueJump >= 0) {
            size_t target;
            if (g_queueJump >= 0) target = (size_t)g_queueJump;
            else target = (qi + queues.size() + g_queueStep) % queues.size();
            g_queueStep = 0; g_queueJump = -1;
            g_switchTarget = (int)target;
            std::cout << ">>> 请求切到队列 " << (target + 1) << "（手动）\n";
        }

        // 淡入淡出状态机：fade>0 时 淡出到黑 → 执行切场 → 淡入；fade=0 硬切
        if (g_switchTarget >= 0 && g_fadeState == 0) {
            if (win.fade > 0.01) {
                g_fadeState = 1; g_fadeT0 = now;
            } else {
                doQueueSwitch((size_t)g_switchTarget);
                g_switchTarget = -1;
                now = glfwGetTime();
            }
        }
        if (g_fadeState == 1) {          // 淡出
            g_fadeAlpha = 1.0 - (now - g_fadeT0) / win.fade;
            if (g_fadeAlpha <= 0.0) {
                g_fadeAlpha = 0.0;
                doQueueSwitch((size_t)g_switchTarget);
                g_switchTarget = -1;
                g_fadeState = 2; g_fadeT0 = glfwGetTime();
                now = g_fadeT0;
            }
        } else if (g_fadeState == 2) {   // 淡入
            g_fadeAlpha = (now - g_fadeT0) / win.fade;
            if (g_fadeAlpha >= 1.0) { g_fadeAlpha = 1.0; g_fadeState = 0; }
        }
        if (g_paused && pauseStart == 0.0) pauseStart = now;
        if (!g_paused && pauseStart != 0.0) { pausedTotal += now - pauseStart; pauseStart = 0.0; }

        double t = now - q0 - pausedTotal;
        if (g_paused) t = pauseStart - q0 - pausedTotal;

        Queue& q = queues[qi];
        if (!g_paused) {
            for (auto& L : q.layers) updateLayer(L, t);

            // 队列结束判定：所有不循环层都播完
            bool anyNonLoop = false, allEnded = true;
            for (const auto& L : q.layers) {
                if (L.loop) continue;
                anyNonLoop = true;
                if (L.state != "ended") allEnded = false;
            }
            if (anyNonLoop && allEnded && g_switchTarget < 0)
                g_switchTarget = (int)((qi + 1) % queues.size());   // 交给淡入淡出状态机执行
        }

        int fbW, fbH;
        glfwGetFramebufferSize(g_win, &fbW, &fbH);
#ifdef HAPPLAYER_HAS_VULKAN
        if (g_useVulkan && vk_render::Renderer::instance().isInitialized()) {
            std::vector<vk_render::Renderer::DrawLayer> vkLayers;
            vkLayers.reserve(queues[qi].layers.size() + 1);
            for (auto& L : queues[qi].layers) {
                if (!L.visible || L.vkTex.view == VK_NULL_HANDLE) continue;
                vk_render::Renderer::DrawLayer dl{};
                dl.imageView = L.vkTex.view;
                dl.tex       = &L.vkTex;
                dl.dirty     = L.vkDirty;
                dl.useHapQ   = L.isHapQ;
                // 布局与 GL 路径 drawLayer 一致：自定义矩形 或 contain 居中
                double va = (double)L.w / L.h;
                double rw, rh, x0, y0;
                double scale = win.scale / 100.0;
                if (L.customRect) {
                    double rw0 = fbW * (L.rw / 100.0);
                    double rh0 = rw0 / va;
                    rw = rw0 * scale; rh = rh0 * scale;
                    x0 = fbW * (L.rx / 100.0) + (rw0 - rw) * 0.5;
                    y0 = fbH * (L.ry / 100.0) + (rh0 - rh) * 0.5;
                } else {
                    double wa = (double)fbW / fbH;
                    if (va > wa) { rw = fbW; rh = fbW / va; }
                    else         { rh = fbH; rw = fbH * va; }
                    rw *= scale; rh *= scale;
                    x0 = (fbW - rw) * 0.5; y0 = (fbH - rh) * 0.5;
                }
                dl.rectX = (float)x0;       dl.rectY = (float)y0;
                dl.rectW = (float)rw;       dl.rectH = (float)rh;
                dl.uvX0 = 0.0f;             dl.uvY0 = 0.0f;
                dl.uvX1 = 1.0f;             dl.uvY1 = 1.0f;
                vkLayers.push_back(dl);
            }
            // 淡入淡出：最顶层盖一张 1x1 黑纹理，alpha = 1-fadeAlpha（黑场渐变等价实现）
            if (g_fadeAlpha < 0.999) {
                auto& R = vk_render::Renderer::instance();
                if (s_vkFadeTex.view == VK_NULL_HANDLE) {
                    uint8_t px[4] = {0, 0, 0, 0};
                    s_vkFadeTex = vk_hapq::createTexture(R.getDevice(), R.getPhysDevice(),
                                                         px, 4, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
                }
                if (s_vkFadeTex.view != VK_NULL_HANDLE) {
                    uint8_t a = (uint8_t)((1.0 - g_fadeAlpha) * 255.0 + 0.5);
                    uint8_t px[4] = {0, 0, 0, a};
                    if (vk_hapq::updateTexture(s_vkFadeTex, px, 4)) {
                        vk_render::Renderer::DrawLayer ov{};
                        ov.imageView = s_vkFadeTex.view;
                        ov.tex       = &s_vkFadeTex;
                        ov.dirty     = true;
                        ov.useHapQ   = false;
                        ov.rectX = 0;        ov.rectY = 0;
                        ov.rectW = (float)fbW; ov.rectH = (float)fbH;
                        ov.uvX0 = 0; ov.uvY0 = 0; ov.uvX1 = 1; ov.uvY1 = 1;
                        vkLayers.push_back(ov);
                    }
                }
            }
            if (vk_render::Renderer::instance().drawFrame(vkLayers)) {
                for (auto& L : queues[qi].layers) L.vkDirty = false;
            }
        } else
#endif
        {
            glViewport(0, 0, fbW, fbH);
            glClear(GL_COLOR_BUFFER_BIT);

            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0, fbW, fbH, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();

            for (const auto& L : queues[qi].layers) drawLayer(L, fbW, fbH, win.scale / 100.0); // 顺序 = 从底到顶
            glDisable(GL_TEXTURE_2D);

            glfwSwapBuffers(g_win);
        }
        glfwPollEvents();
        frames++;

        // --- 自定义 frame pacing：精确控帧，避免 vsync 抖动 ---
        // 用 g_maxFps 限制最高帧率（默认 60）；sleep 到目标时间，剩 500us 内忙等
        if (g_maxFps > 0.1) {
            const double frameDur = 1.0 / g_maxFps;
            const double frameStart = now;   // 此帧的逻辑起点
            const double renderEnd  = glfwGetTime();
            double remaining = frameDur - (renderEnd - frameStart);
            if (remaining > 0.0) {
                auto endTime = std::chrono::steady_clock::now()
                             + std::chrono::duration<double>(remaining);
                // 大于 1ms 就 sleep_for（让出 CPU），最后 500us 忙等保证精度
                while (std::chrono::steady_clock::now() < endTime) {
                    double left = std::chrono::duration<double>(
                        endTime - std::chrono::steady_clock::now()).count();
                    if (left > 0.001) {
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    } else {
                        std::this_thread::yield();   // 忙等到精确时间
                    }
                }
            }
        }

        // FPS 统计（0.5s 窗口）
        if (now - fpsWinStart >= 0.5) {
            fps = frames / (now - fpsWinStart);
            frames = 0;
            fpsWinStart = now;
            int active = 0;
            for (const auto& L : queues[qi].layers) if (L.state == "playing") active++;

            // P1.2 自适应帧率：检测负载，自动降帧保稳定
            // 如果实测 fps < 目标 fps 的 60%，降一档；如果 > 95%，升一档（回到初始值）
            // 范围：[15, g_maxFps]，步长 5fps
            static double g_initialMaxFps = 0;   // 启动时记录初始值
            if (g_initialMaxFps == 0.0) g_initialMaxFps = g_maxFps;
            const double targetFps = (g_maxFps > 0.1) ? g_maxFps : 30.0;
            if (fps < targetFps * 0.60 && g_maxFps > 30.0) {
                g_maxFps -= 5.0;
                if (g_maxFps < 30.0) g_maxFps = 30.0;   // 下限 30：降太低在展项上已无意义，且难恢复
                std::cout << "[自适应] 负载高 (" << (int)fps << " fps < 目标"
                          << (int)targetFps << ")，maxFps 降至 " << (int)g_maxFps << "\n";
            } else if (fps > targetFps * 0.95 && g_maxFps < g_initialMaxFps) {
                g_maxFps += 5.0;
                if (g_maxFps > g_initialMaxFps) g_maxFps = g_initialMaxFps;
                std::cout << "[自适应] 负载恢复 (" << (int)fps << " fps)，maxFps 升至 "
                          << (int)g_maxFps << "\n";
            }

            char title[192];
            std::snprintf(title, sizeof(title), "happlayer | FPS %.0f | 队列 %zu/%zu | 活动 %d/%zu | t=%.1fs%s",
                          fps, qi + 1, queues.size(), active, queues[qi].layers.size(),
                          t, g_paused ? " | 已暂停" : "");
            glfwSetWindowTitle(g_win, title);
        }
        // 控制台每秒一行状态
        if (now - statLine >= 1.0) {
            statLine = now;
            std::ostringstream ss;
            ss << "[队列" << (qi + 1) << " t=" << (long)(t * 10) / 10.0 << "s] fps=" << (long)(fps + 0.5);
            for (const auto& L : queues[qi].layers) {
                ss << " | " << L.name << ":" << L.state << "(#" << L.shownIndex << ")";
                if (std::getenv("HAPPLAYER_DEBUG_RING")) {   // 排障：环形队列/包队列/轮次/播放头
                    std::lock_guard<std::shared_mutex> dlk(L.dec->mtx);
                    std::lock_guard<std::mutex> plk(L.dec->pktMtx);
                    ss << "[r" << L.dec->ring.size()
                       << " f" << (L.dec->ring.empty() ? -1 : (long)L.dec->ring.front().index)
                       << " fg" << (L.dec->ring.empty() ? 0 : (long)L.dec->ring.front().gen)
                       << " pq" << L.dec->pktQueue.size()
                       << " g" << L.dec->gen
                       << " pi" << (long)L.dec->presentIndex.load()
                       << " ni" << (long)L.dec->nextIndexDbg.load()
                       << " eof" << (L.dec->decoderEof ? 1 : 0) << "]";
                }
            }
            std::cout << ss.str() << std::endl;

            // 配置热重载：每 1 秒检查 layers.txt mtime
            if (g_configMtime != 0 && now - g_nextHotReloadCheck >= 1.0) {
                g_nextHotReloadCheck = now;
                time_t cur = getFileMtime(configPath);
                if (cur != 0 && cur != g_configMtime) {
                    std::cout << "[热重载] 检测到 layers.txt 变更 ("
                              << g_configMtime << " → " << cur << ")\n";
                    g_configMtime = cur;
                    if (reloadMedia(queues, configPath, win, qi)) {
                        q0 = glfwGetTime(); pausedTotal = 0.0; t = 0.0;
                        now = q0;
                    }
                }
            }

            // 健康监控 1：fps < 5 持续 5 秒 → 主动退出（外部进程拉起）
            if (fps > 0.5 && fps < 5.0) {
                if (g_fpsBelowSince == 0) g_fpsBelowSince = now;
                else if (now - g_fpsBelowSince >= 5.0) {
                    std::cerr << "[健康监控] 帧率持续过低 (" << (int)fps
                              << " fps 持续 " << (now - g_fpsBelowSince)
                              << " 秒)，主动退出等待重启\n";
                    if (g_win) glfwSetWindowShouldClose(g_win, GLFW_TRUE);
                }
            } else {
                g_fpsBelowSince = 0;
            }

            // 健康监控 2：启动超 7 天 → 主动退出（防内存泄漏累积）
            auto upSecs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - g_startTime).count();
            if (upSecs > 7 * 24 * 3600.0) {
                std::cerr << "[健康监控] 已运行 " << (long)(upSecs / 3600)
                          << " 小时，主动退出（防止内存泄漏累积）\n";
                glfwSetWindowShouldClose(g_win, GLFW_TRUE);
            }
        }
        if (benchSeconds > 0.0 && now - benchStart >= benchSeconds) {
            std::cout << "\n[bench] " << benchSeconds << "s 结束，平均 FPS ≈ "
                      << (long)(fps + 0.5) << "\n";
            for (size_t i = 0; i < queues.size(); ++i)
                for (const auto& L : queues[i].layers)
                    std::cout << "  队列" << (i + 1) << " " << L.name
                              << ": 解码 " << (L.dec ? L.dec->framesDecoded : 0) << " 帧\n";
            // 小项 1：JSON bench 报告（CI/CD 解析用）
            if (g_jsonBench) {
                std::cout << "{\n";
                std::cout << "  \"duration_sec\": " << benchSeconds << ",\n";
                std::cout << "  \"avg_fps\": " << (long)(fps + 0.5) << ",\n";
                std::cout << "  \"layers\": [\n";
                bool firstLayer = true;
                for (size_t i = 0; i < queues.size(); ++i) {
                    for (const auto& L : queues[i].layers) {
                        if (!firstLayer) std::cout << ",\n";
                        firstLayer = false;
                        uint64_t decoded = L.dec ? L.dec->framesDecoded : 0;
                        std::cout << "    {\"queue\": " << (i + 1)
                                  << ", \"name\": \"" << L.name << "\""
                                  << ", \"frames_decoded\": " << decoded
                                  << ", \"state\": \"" << L.state << "\""
                                  << "}";
                    }
                }
                std::cout << "\n  ],\n";
                // audio drift 计数是 audio::Context 的 private 字段（禁止改 audio.h）
                // 这里只暴露 audio_enabled，drift 详细数据见 stderr 的 "[audio] 已关闭" 行
                bool audioOn = audio::Context::instance().ready();
                std::cout << "  \"audio\": {\"enabled\": " << (audioOn ? "true" : "false") << "}\n";
                std::cout << "}\n";
                std::cout.flush();
            }
            break;
        }
    }

    for (auto& q : queues)
        for (auto& L : q.layers) closeLayer(L);
    if (g_udp != INVALID_SOCKET) { closesocket(g_udp); WSACleanup(); }
    audio::Context::instance().shutdown();   // 音频：停 SDL 设备 + 解码线程
    // 小项 3：退出时保存窗口位置到 state.json（非全屏模式）
    // B-21 修复：运行时按 F 进了全屏再退出，存的是全屏几何（下次启动位置异常）。
    // 全屏状态下改为保存进入全屏前记忆的窗口化几何。
    if (g_win && g_fullscreen) {
        saveWindowState(g_winX, g_winY, g_winW, g_winH);
    } else if (g_win && !win.fullscreen) {
        int fx, fy;
        glfwGetWindowPos(g_win, &fx, &fy);
        int fw, fh;
        glfwGetWindowSize(g_win, &fw, &fh);
        saveWindowState(fx, fy, fw, fh);
        LOG(2, "[窗口] 已保存位置 (" << fx << "," << fy << ") " << fw << "x" << fh << "\n");
    }
#ifdef HAPPLAYER_HAS_VULKAN
    // 必须在 glfwDestroyWindow 之前：surface 绑在 HWND 上
    vk_render::Renderer::instance().shutdown();
#endif
    glfwDestroyWindow(g_win);
    glfwTerminate();
    timeEndPeriod(1);
    // 开机自启三件套 — 防休眠：还原系统默认的睡眠/显示器策略
    SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}
