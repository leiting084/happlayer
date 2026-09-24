// 临时工具：dump HAP Q 文件第 0 帧的 DXT5 alpha 端点（诊断 YS 端点交换假说）
#include <cstdio>
#include <cstdint>
#include <vector>
#include <snappy.h>
#include "../src/hapdecode.h"

extern "C" {
#include <libavformat/avformat.h>
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: dump_hap <file.mov> <宽>\n"); return 1; }
    const char* path = argv[1];
    int W = atoi(argv[2]);

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) { fprintf(stderr, "open fail\n"); return 1; }
    avformat_find_stream_info(fmt, nullptr);
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    AVPacket* pkt = av_packet_alloc();
    std::vector<uint8_t> dxt, dxt2;
    hap::PixFmt pfmt; std::string err; bool got = false;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
        got = hap::decodeInto(pkt->data, pkt->size, dxt, pfmt, err, &dxt2);
        av_packet_unref(pkt);
        break;
    }
    printf("decode=%d fmt=%d dxtSize=%zu\n", got, (int)pfmt, dxt.size());
    if (!got) return 1;

    // DXT5 block = 16B：a0,a1,idx(6B),c0(2B),c1(2B),cidx(4B)。宽 W → W/4 块/行
    int blocksPerRow = W / 4;
    for (int b = 0; b < blocksPerRow; ++b) {
        const uint8_t* blk = dxt.data() + (size_t)b * 16;
        printf("块(%d,0): a0=%3d a1=%3d aidx=%02x%02x%02x%02x%02x%02x  c0=%02x%02x c1=%02x%02x\n",
               b, blk[0], blk[1],
               blk[2], blk[3], blk[4], blk[5], blk[6], blk[7],
               blk[8], blk[9], blk[10], blk[11]);
    }
    return 0;
}
