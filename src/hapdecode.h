// HAP 帧解码：把 MOV 里的 HAP packet 解成 DXT 压缩纹理数据。
// 依据 Vidvox HAP 规范：https://github.com/Vidvox/hap
// 支持 Hap (RGB DXT1) 与 Hap Alpha (RGBA DXT5)，含 snappy 与多 chunk 两种情况。
// Hap Q (YCoCg DXT5) 也支持解码：GPU 把它当标准 DXT5 上传，
//   由调用方在 shader 中做 YCoCg→RGB 转换（GPU 看不到色域差异）。
// 仍不支持 BC7 / RGTC —— 解码时会报明确错误。
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <snappy.h>

namespace hap {

// GPU 格式：DXT1/DXT5 上传到 GPU 是相同处理流程（压缩纹理 subimage）
// YCoCg_DXT5 与 DXT5 的 GPU 格式相同，区别只在解码后的色彩空间。
// RGTC1 = BC4 单通道，Hap Q Alpha 的 alpha 平面。
enum class PixFmt { DXT1, DXT5, YCoCg_DXT5, RGTC1 };

struct DecodeResult {
    bool ok = false;
    std::string err;
    PixFmt fmt = PixFmt::DXT5;
    std::vector<uint8_t> data; // DXT 压缩纹理数据
};

// 读取 section 头（4 或 8 字节），p/remain 前移到头之后
inline bool readSectionHeader(const uint8_t*& p, size_t& remain, uint32_t& type, uint32_t& size) {
    if (remain < 4) return false;
    if (p[0] == 0 && p[1] == 0 && p[2] == 0) {
        if (remain < 8) return false;
        type = p[3];
        size = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        p += 8; remain -= 8;
    } else {
        type = p[3];
        size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        p += 4; remain -= 4;
    }
    return size <= remain;
}

inline bool snappyDecompress(const uint8_t* src, size_t srcLen,
                             std::vector<uint8_t>& out, size_t outOffset,
                             std::string& err) {
    size_t len = 0;
    if (!snappy::GetUncompressedLength(reinterpret_cast<const char*>(src), srcLen, &len)) {
        err = "snappy: 无法读取解压长度";
        return false;
    }
    out.resize(outOffset + len);
    if (!snappy::RawUncompress(reinterpret_cast<const char*>(src), srcLen,
                               reinterpret_cast<char*>(out.data() + outOffset))) {
        err = "snappy: 解压失败";
        return false;
    }
    return true;
}

inline uint32_t readLE32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 解码单个图像 section 的数据部分（type 已知，p/size 为 section 数据）
inline bool decodeSectionData(uint32_t type, const uint8_t* p, size_t size,
                              std::vector<uint8_t>& out, PixFmt& fmt, std::string& err) {
    out.clear();
    const uint8_t pix  = type & 0x0F;        // 低半字节：像素格式
    const uint8_t comp = (type >> 4) & 0x0F; // 高半字节：二段压缩方式

    if (pix == 0xB)      fmt = PixFmt::DXT1;
    else if (pix == 0xE) fmt = PixFmt::DXT5;
    else if (pix == 0xF) fmt = PixFmt::YCoCg_DXT5;  // Hap Q — YCoCg 色域，需 shader 转换
    else if (pix == 0x1) fmt = PixFmt::RGTC1;       // BC4 — Hap Q Alpha 的 alpha 平面
    else {
        err = "暂不支持该像素格式（仅支持 Hap/Hap Alpha/Hap Q/Hap Q Alpha，BC7 等未实现）";
        return false;
    }

    if (comp == 0xA) {          // 无压缩：section 数据即纹理数据
        out.assign(p, p + size);
        return true;
    }
    if (comp == 0xB) {          // 整体 snappy
        return snappyDecompress(p, size, out, 0, err);
    }
    if (comp == 0xC) {          // 多 chunk：先读 decode instructions
        const uint8_t* ip = p;
        size_t iremain = size;
        uint32_t itype = 0, isize = 0;
        if (!readSectionHeader(ip, iremain, itype, isize) || itype != 0x01) {
            err = "HAP 多 chunk 帧：缺少 decode instructions";
            return false;
        }
        const uint8_t* frameData = ip + isize; // chunk 数据紧跟 instructions 容器之后

        std::vector<uint8_t>  compressors;
        std::vector<uint32_t> sizes, offsets;
        bool hasOffsets = false;

        const uint8_t* cp = ip;
        size_t cremain = isize;
        while (cremain > 0) {
            uint32_t stype = 0, ssize = 0;
            if (!readSectionHeader(cp, cremain, stype, ssize)) {
                err = "HAP decode instructions 解析失败";
                return false;
            }
            if (stype == 0x02) {           // compressor table
                compressors.assign(cp, cp + ssize);
            } else if (stype == 0x03) {    // size table
                for (uint32_t i = 0; i + 4 <= ssize; i += 4)
                    sizes.push_back(readLE32(cp + i));
            } else if (stype == 0x04) {    // offset table（可选）
                hasOffsets = true;
                for (uint32_t i = 0; i + 4 <= ssize; i += 4)
                    offsets.push_back(readLE32(cp + i));
            }
            cp += ssize; cremain -= ssize;
        }

        const size_t n = sizes.size();
        if (n == 0 || compressors.size() != n || (hasOffsets && offsets.size() != n)) {
            err = "HAP chunk 表不一致";
            return false;
        }

        uint32_t running = 0;
        size_t outOff = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t off = hasOffsets ? offsets[i] : running;
            running = off + sizes[i];
            const uint8_t* chunk = frameData + off;
            if (compressors[i] == 0x0A) {
                out.resize(outOff + sizes[i]);
                memcpy(out.data() + outOff, chunk, sizes[i]);
                outOff += sizes[i];
            } else if (compressors[i] == 0x0B) {
                if (!snappyDecompress(chunk, sizes[i], out, outOff, err))
                    return false;
                outOff = out.size();
            } else {
                err = "HAP chunk 使用了未知压缩器";
                return false;
            }
        }
        return true;
    }

    err = "HAP 帧类型未知";
    return false;
}

// 解码一帧 HAP 到 out（复用 out 的容量，避免每帧分配大块内存）
// 多图组合帧（0x0D，Hap Q Alpha）：颜色解到 out，alpha 平面（BC4）解到 out2
inline bool decodeInto(const uint8_t* src, size_t srcSize,
                       std::vector<uint8_t>& out, PixFmt& fmt, std::string& err,
                       std::vector<uint8_t>* out2 = nullptr) {
    if (out2) out2->clear();
    const uint8_t* p = src;
    size_t remain = srcSize;
    uint32_t type = 0, size = 0;

    if (!readSectionHeader(p, remain, type, size)) {
        err = "HAP section 头解析失败";
        return false;
    }

    if (type != 0x0D)
        return decodeSectionData(type, p, size, out, fmt, err);

    // 多图组合帧：section 数据 = 两个子 section（YCoCg DXT5 + RGTC1 alpha）
    if (!out2) {
        err = "Hap Q Alpha 帧需要第二输出缓冲";
        return false;
    }
    const uint8_t* sp = p;
    size_t sremain = size;
    bool gotColor = false, gotAlpha = false;
    while (sremain > 0) {
        uint32_t stype = 0, ssize = 0;
        // readSectionHeader 内部已从 sremain 减掉头部，这里只再减数据部分
        if (!readSectionHeader(sp, sremain, stype, ssize)) {
            err = "HAP 多图帧：子 section 头解析失败";
            return false;
        }
        uint8_t subPix = stype & 0x0F;
        if (subPix == 0xF) {                         // YCoCg DXT5 颜色
            if (!decodeSectionData(stype, sp, ssize, out, fmt, err)) return false;
            gotColor = true;
        } else if (subPix == 0x1) {                  // RGTC1 alpha
            PixFmt afmt;
            if (!decodeSectionData(stype, sp, ssize, *out2, afmt, err)) return false;
            gotAlpha = true;
        }   // 未知子 section 按规范跳过
        sp += ssize; sremain -= ssize;
    }
    if (!gotColor || !gotAlpha) {
        err = "HAP 多图帧：缺少颜色或 alpha 子图";
        return false;
    }
    return true;
}

inline DecodeResult decode(const uint8_t* src, size_t srcSize) {
    DecodeResult r;
    r.ok = decodeInto(src, srcSize, r.data, r.fmt, r.err);
    return r;
}

} // namespace hap
