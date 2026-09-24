// audio.h — 音频播放子系统（SDL2，多源混音）
//
// 支持多个音频源同时混音：
//   - 每个带音频流的 HAP 层 = 一个源（音量取图层行第 7 字段 |音量%）
//   - [audio] source 外挂音频 = 一个源（alwaysOn，不受层状态控制）
// 同步策略：视频（队列时钟）为主时钟，每个源独立校正，允许 ±50ms 漂移。
//   - 源落后 → 从 ring 丢弃 50ms PCM 向前追
//   - 源超前 <200ms 不处理；>200ms（通常切场后）硬对齐
// 线程约定：每个源的 fmt_/codecCtx_ 只由自己的解码线程访问；切场 seek 用
//   restart 信号让解码线程自己执行（AVFormatContext 不是线程安全的）。
//
// 数据流：
//   每源 decodeLoop: av_read_frame → swr_convert → 该源 PCM ring
//   SDL 回调: 逐源 pull → 各乘音量 → int32 累加 → 截幅 → 输出
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;

namespace audio {

// [audio] 段解析用（外挂单源）
struct Config {
    std::string source;
    float volume = 1.0f;
    bool  loop = false;
};

struct SourceSpec {
    std::string path;       // 文件路径
    int streamIdx = -1;     // >=0：该文件的音频流索引（HAP 内嵌）；-1：独立音频文件
    float volume = 1.0f;    // 0~1
    bool loop = false;
    bool alwaysOn = false;  // true=外挂音频（只受全局暂停控制）；false=跟随层 playing 状态
};

class Context {
public:
    static Context& instance();

    // 多源初始化：内嵌音频层（可多个）+ 可选外挂音频，混音输出
    bool initSources(const std::vector<SourceSpec>& specs, double queueClock0);
    // 兼容旧单源接口
    bool init(const Config& cfg, double queueClock0);
    bool initFromLayer(const std::string& hapPath, int audioStreamIdx,
                       float volume, bool loop, double queueClock0);

    // 主线程每帧：用队列时钟校正各源进度；paused 控制暂停/恢复
    void tick(double queueClock, bool paused);
    // 切场：所有源回 0（解码线程自行 seek）
    void onQueueStart(double queueClock0);
    // 主线程同步层状态（每层每帧；内嵌源只在层 playing 时发声）
    void setSourcePlaying(const std::string& path, bool playing);

    void shutdown();
    void toggleMute();
    void setMasterVolume(float v) { masterGain_.store(v); }   // UDP /volume 或 OSC /volume

    bool ready() const;
    bool muted() const;
    float volume() const;

    // 音频参数（回调混音用）
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels   = 2;
    static constexpr int kBytesPerSample = 2;

private:
    Context() = default;
    ~Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    friend void sdlAudioCallback(void* userdata, Uint8* stream, int len);

    // ---------------- 音频源 ----------------
    struct Source {
        SourceSpec spec;
        // 文件与解码（仅本源的解码线程访问）
        AVFormatContext* fmt = nullptr;
        int audioStreamIdx = -1;
        AVCodecContext* codecCtx = nullptr;
        SwrContext* swr = nullptr;
        // 解码线程
        std::thread th;
        std::atomic<bool> exiting{false};
        std::atomic<uint64_t> restart{0};   // 切场信号（onQueueStart 递增）
        // PCM ring（存采样点 int16 个数）
        std::mutex ringMtx;
        std::vector<int16_t> ringBuf;
        size_t ringRead = 0, ringWrite = 0, ringCount = 0;
        std::atomic<bool> eof{false};
        // 播放状态与进度（audioPos 由回调线程推进 + tick 修正）
        std::atomic<bool> playing{false};
        std::atomic<double> audioPos{0.0};
        std::atomic<uint64_t> driftDrops{0}, driftSnaps{0};
    };

    bool openSource(Source& s);
    void closeSource(Source& s);
    void decodeLoop(Source& s);

    // ring 操作（采样点单位）
    static void ringPush(Source& s, const int16_t* src, size_t n);
    static size_t ringPull(Source& s, int16_t* dst, size_t n);
    static size_t ringDiscard(Source& s, size_t n);
    static void ringClear(Source& s);

    // SDL 回调调用：混音 nSamples 个采样点到 dst
    void mix(int16_t* dst, size_t nSamples);

    static constexpr size_t kRingCapSamples = 48000 * 2 * 2;  // 约 2 秒立体声

    std::vector<std::unique_ptr<Source>> sources_;

    // SDL
    uint32_t sdlDeviceId_ = 0;
    bool sdlOpened_ = false;
    std::atomic<bool> muted_{false};
    std::atomic<float> masterGain_{1.0f};   // 总音量（所有源共用，UDP/OSC 可调）

    // 同步
    std::atomic<double> queueClock0_{0.0};  // 恒 0（统一用队列相对时间），保留兼容
};

} // namespace audio
