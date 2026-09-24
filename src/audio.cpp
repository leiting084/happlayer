// audio.cpp — 音频子系统实现（多源混音）
//
//   1. 每源一个解码线程：av_read_frame → swr_convert（→48kHz s16 stereo）→ 该源 PCM ring
//   2. SDL 回调：逐源 pull → 各乘音量 → int32 累加 → 截幅到 int16 → 输出
//   3. 主线程 tick：队列时钟为主时钟，逐源做 ±50ms 漂移校正（落后丢 50ms 追，
//      超前 >200ms 才硬对齐）
//   4. 切场：onQueueStart 给所有源发 restart 信号，解码线程自己 seek（fmt 非线程安全）

#include "audio.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <iostream>

namespace audio {

Context& Context::instance() {
    static Context ctx;
    return ctx;
}

bool Context::ready() const { return sdlDeviceId_ != 0; }
bool Context::muted()  const { return muted_.load(); }
float Context::volume() const { return 1.0f; }   // 音量在各源上（spec.volume）

// ---------------- ring（采样点单位） ----------------
void Context::ringPush(Source& s, const int16_t* src, size_t n) {
    std::lock_guard<std::mutex> lk(s.ringMtx);
    if (s.ringBuf.empty()) {
        s.ringBuf.resize(kRingCapSamples);
        s.ringRead = s.ringWrite = 0;
    }
    size_t cap = s.ringBuf.size();
    for (size_t i = 0; i < n; ++i) {
        if (s.ringCount >= cap) break;   // 满了丢新的（消费端落后由 drift 修正处理）
        s.ringBuf[s.ringWrite] = src[i];
        s.ringWrite = (s.ringWrite + 1) % cap;
        s.ringCount++;
    }
}

size_t Context::ringPull(Source& s, int16_t* dst, size_t n) {
    std::lock_guard<std::mutex> lk(s.ringMtx);
    size_t cap = s.ringBuf.size();
    if (cap == 0) return 0;
    size_t got = 0;
    while (got < n && s.ringCount > 0) {
        dst[got++] = s.ringBuf[s.ringRead];
        s.ringRead = (s.ringRead + 1) % cap;
        s.ringCount--;
    }
    return got;
}

size_t Context::ringDiscard(Source& s, size_t n) {
    std::lock_guard<std::mutex> lk(s.ringMtx);
    if (s.ringBuf.empty() || s.ringCount == 0) return 0;
    size_t dropped = std::min(n, s.ringCount);
    s.ringRead = (s.ringRead + dropped) % s.ringBuf.size();
    s.ringCount -= dropped;
    return dropped;
}

void Context::ringClear(Source& s) {
    std::lock_guard<std::mutex> lk(s.ringMtx);
    s.ringRead = s.ringWrite = s.ringCount = 0;
}

// ---------------- 混音（SDL 回调线程） ----------------
void Context::mix(int16_t* dst, size_t nSamples) {
    static thread_local int32_t acc[8192];
    static thread_local int16_t tmp[8192];
    if (nSamples > 8192) nSamples = 8192;
    std::memset(acc, 0, nSamples * sizeof(int32_t));

    const bool muted = muted_.load();
    const float master = masterGain_.load();
    for (auto& sp : sources_) {
        Source& s = *sp;
        if (muted || !s.playing.load()) continue;
        size_t pulled = ringPull(s, tmp, nSamples);
        if (pulled == 0) continue;
        const float vol = s.spec.volume * master;
        for (size_t i = 0; i < pulled; ++i)
            acc[i] += (int32_t)(tmp[i] * vol);
        // 推进该源的已播位置（采样点 ÷ 声道数 = 每声道帧数）
        s.audioPos.store(s.audioPos.load() +
                         (double)pulled / kChannels / kSampleRate);
    }

    for (size_t i = 0; i < nSamples; ++i) {
        int32_t v = acc[i];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
    }
}

// ---------------- SDL 回调 ----------------
void sdlAudioCallback(void* /*userdata*/, Uint8* stream, int len) {
    Context& ctx = Context::instance();
    ctx.mix(reinterpret_cast<int16_t*>(stream), (size_t)len / sizeof(int16_t));
}

// ---------------- 源开关 ----------------
bool Context::openSource(Source& s) {
    const char* path = s.spec.path.c_str();
    if (avformat_open_input(&s.fmt, path, nullptr, nullptr) < 0) {
        std::cerr << "[audio] 打不开: " << s.spec.path << "\n";
        return false;
    }
    if (avformat_find_stream_info(s.fmt, nullptr) < 0) {
        std::cerr << "[audio] 读取流信息失败: " << s.spec.path << "\n";
        avformat_close_input(&s.fmt);
        s.fmt = nullptr;
        return false;
    }
    s.audioStreamIdx = (s.spec.streamIdx >= 0)
        ? s.spec.streamIdx
        : av_find_best_stream(s.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s.audioStreamIdx < 0 || (int)s.fmt->nb_streams <= s.audioStreamIdx) {
        std::cerr << "[audio] 没有音频流: " << s.spec.path << "\n";
        avformat_close_input(&s.fmt);
        s.fmt = nullptr;
        return false;
    }
    AVStream* st = s.fmt->streams[s.audioStreamIdx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        std::cerr << "[audio] 找不到解码器: " << s.spec.path << "\n";
        avformat_close_input(&s.fmt);
        s.fmt = nullptr;
        return false;
    }
    s.codecCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(s.codecCtx, st->codecpar);
    if (avcodec_open2(s.codecCtx, dec, nullptr) < 0) {
        std::cerr << "[audio] 打开解码器失败: " << s.spec.path << "\n";
        avcodec_free_context(&s.codecCtx);
        avformat_close_input(&s.fmt);
        s.fmt = nullptr;
        return false;
    }

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    int srcCh = s.codecCtx->ch_layout.nb_channels > 0 ? s.codecCtx->ch_layout.nb_channels : 2;
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, srcCh);
    int ret = swr_alloc_set_opts2(&s.swr,
        &outLayout, AV_SAMPLE_FMT_S16, kSampleRate,
        &inLayout, s.codecCtx->sample_fmt, s.codecCtx->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (ret < 0 || !s.swr || swr_init(s.swr) < 0) {
        std::cerr << "[audio] swr 初始化失败: " << s.spec.path << "\n";
        if (s.swr) swr_free(&s.swr);
        avcodec_free_context(&s.codecCtx);
        avformat_close_input(&s.fmt);
        s.fmt = nullptr;
        return false;
    }

    std::cout << "[audio] 源已加载: " << s.spec.path
              << "  " << s.codecCtx->sample_rate << "Hz "
              << av_get_sample_fmt_name(s.codecCtx->sample_fmt)
              << " ch=" << srcCh << " → 48kHz s16 stereo"
              << "  音量=" << s.spec.volume
              << "  循环=" << (s.spec.loop ? "是" : "否")
              << (s.spec.alwaysOn ? "  [外挂]" : "  [随层]") << "\n";
    return true;
}

void Context::closeSource(Source& s) {
    if (s.swr) { swr_close(s.swr); swr_free(&s.swr); s.swr = nullptr; }
    if (s.codecCtx) { avcodec_free_context(&s.codecCtx); s.codecCtx = nullptr; }
    if (s.fmt) { avformat_close_input(&s.fmt); s.fmt = nullptr; }
    s.audioStreamIdx = -1;
}

// ---------------- 解码线程（每源一个） ----------------
void Context::decodeLoop(Source& s) {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> resampleBuf;
    uint8_t* outArr[1] = { nullptr };
    uint64_t seenRestart = s.restart.load();

    while (!s.exiting.load()) {
        // 切场请求：自己执行 seek（fmt 单线程访问）
        uint64_t rs = s.restart.load();
        if (rs != seenRestart) {
            seenRestart = rs;
            av_seek_frame(s.fmt, s.audioStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
            avformat_flush(s.fmt);
            avcodec_flush_buffers(s.codecCtx);
            s.eof.store(false);
            continue;
        }

        int ret = av_read_frame(s.fmt, pkt);
        if (ret < 0) {
            if (s.spec.loop) {
                av_seek_frame(s.fmt, s.audioStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
                avformat_flush(s.fmt);
                avcodec_flush_buffers(s.codecCtx);
                continue;
            }
            // 非循环播完：挂起等切场信号
            s.eof.store(true);
            while (!s.exiting.load() && s.restart.load() == seenRestart)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (pkt->stream_index != s.audioStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(s.codecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(s.codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int outSamples = swr_get_out_samples(s.swr, frame->nb_samples);
            size_t needBytes = (size_t)outSamples * kChannels * kBytesPerSample + 64;
            if (resampleBuf.size() < needBytes) resampleBuf.resize(needBytes);

            outArr[0] = resampleBuf.data();
            int converted = swr_convert(s.swr,
                outArr, outSamples,
                (const uint8_t* const*)frame->extended_data, frame->nb_samples);
            if (converted > 0) {
                // converted 是「每声道采样数」，交错立体声 ×声道数
                ringPush(s, reinterpret_cast<const int16_t*>(resampleBuf.data()),
                         (size_t)converted * kChannels);
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

// ---------------- 初始化 ----------------
bool Context::initSources(const std::vector<SourceSpec>& specs, double queueClock0) {
    (void)queueClock0;   // 统一用队列相对时间，起点恒 0
    queueClock0_.store(0.0);
    if (specs.empty()) {
        std::cout << "[audio] 无音频源，跳过音频初始化\n";
        return false;
    }

    // 打开所有源（失败的剔除）
    for (const auto& spec : specs) {
        auto s = std::make_unique<Source>();
        s->spec = spec;
        if (openSource(*s)) sources_.push_back(std::move(s));
    }
    if (sources_.empty()) {
        std::cerr << "[audio] 所有音频源都打不开，无音频\n";
        return false;
    }

    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        std::cerr << "[audio] SDL_Init(AUDIO) 失败: " << SDL_GetError() << "\n";
        sources_.clear();
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples = 2048;
    want.callback = sdlAudioCallback;
    want.userdata = this;
    SDL_AudioSpec have{};
    sdlDeviceId_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (sdlDeviceId_ == 0) {
        std::cerr << "[audio] SDL_OpenAudioDevice 失败: " << SDL_GetError() << "\n";
        sources_.clear();
        return false;
    }
    (void)have;
    SDL_PauseAudioDevice(sdlDeviceId_, 0);
    sdlOpened_ = true;
    muted_.store(false);

    for (auto& s : sources_) {
        if (s->spec.alwaysOn) s->playing.store(true);   // 外挂源常开；随层源由 setSourcePlaying 驱动
        s->th = std::thread(&Context::decodeLoop, this, std::ref(*s));
    }

    std::cout << "[audio] 已启动: " << sources_.size() << " 个源混音输出\n";
    return true;
}

// 兼容旧接口
bool Context::init(const Config& cfg, double queueClock0) {
    SourceSpec spec;
    spec.path = cfg.source;
    spec.streamIdx = -1;
    spec.volume = cfg.volume;
    spec.loop = cfg.loop;
    spec.alwaysOn = true;
    return initSources({spec}, queueClock0);
}

bool Context::initFromLayer(const std::string& hapPath, int audioStreamIdx,
                            float volume, bool loop, double queueClock0) {
    SourceSpec spec;
    spec.path = hapPath;
    spec.streamIdx = audioStreamIdx;
    spec.volume = volume;
    spec.loop = loop;
    spec.alwaysOn = true;   // 旧单源行为：内嵌源始终播
    return initSources({spec}, queueClock0);
}

void Context::setSourcePlaying(const std::string& path, bool playing) {
    for (auto& s : sources_)
        if (!s->spec.alwaysOn && s->spec.path == path)
            s->playing.store(playing);
}

void Context::tick(double queueClock, bool paused) {
    if (!ready()) return;

    if (paused) {
        if (sdlOpened_) {
            SDL_PauseAudioDevice(sdlDeviceId_, 1);
            sdlOpened_ = false;
        }
        return;
    }
    if (!sdlOpened_ && sdlDeviceId_) {
        SDL_PauseAudioDevice(sdlDeviceId_, 0);
        sdlOpened_ = true;
    }

    double vt = queueClock - queueClock0_.load();
    constexpr double kThreshold = 0.050;                 // 50ms 起调
    constexpr size_t kStepSamples = 2400 * kChannels;    // 50ms 的采样点数

    for (auto& sp : sources_) {
        Source& s = *sp;
        if (!s.playing.load()) continue;
        double drift = vt - s.audioPos.load();
        if (drift > kThreshold) {
            // 源落后：丢 50ms 向前追
            size_t dropped = ringDiscard(s, kStepSamples);
            s.audioPos.store(s.audioPos.load() +
                             (double)dropped / kChannels / kSampleRate);
            s.driftDrops.fetch_add(1);
        } else if (drift < -kThreshold * 4) {
            // 源超前 >200ms（通常切场/严重卡顿后）：硬对齐
            s.audioPos.store(vt);
            s.driftSnaps.fetch_add(1);
        }
    }
}

void Context::onQueueStart(double queueClock0) {
    (void)queueClock0;
    queueClock0_.store(0.0);
    for (auto& s : sources_) {
        s->audioPos.store(0.0);
        s->driftDrops.store(0);
        s->driftSnaps.store(0);
        ringClear(*s);
        s->eof.store(false);
        s->restart.fetch_add(1);   // 解码线程自己 seek
    }
}

void Context::shutdown() {
    for (auto& s : sources_) {
        s->exiting.store(true);
        if (s->th.joinable()) s->th.join();
        uint64_t totalDrops = s->driftDrops.load(), totalSnaps = s->driftSnaps.load();
        if (totalDrops || totalSnaps)
            std::cout << "[audio] 源 " << s->spec.path
                      << " driftDrops=" << totalDrops << " driftSnaps=" << totalSnaps << "\n";
        closeSource(*s);
    }
    sources_.clear();

    if (sdlDeviceId_) {
        SDL_CloseAudioDevice(sdlDeviceId_);
        sdlDeviceId_ = 0;
    }
    sdlOpened_ = false;
    std::cout << "[audio] 已关闭\n";
}

void Context::toggleMute() {
    bool nowMuted = !muted_.load();
    muted_.store(nowMuted);
    std::cout << "[audio] " << (nowMuted ? "静音" : "取消静音") << "\n";
}

} // namespace audio
