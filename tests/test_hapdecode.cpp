// HAP 解码器单元测试：构造合法帧 → decode → 与原始 DXT 数据比对
#include <cstdio>
#include <cstring>
#include <vector>
#include <snappy.h>
#include "../src/hapdecode.h"

static void appendHeader(std::vector<uint8_t>& v, uint8_t type, uint32_t size) {
    v.push_back(size & 0xFF);
    v.push_back((size >> 8) & 0xFF);
    v.push_back((size >> 16) & 0xFF);
    v.push_back(type);
}

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;
}

static bool same(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

int main() {
    // 64x64 DXT5 数据 = 4096 字节，填充可识别图案
    std::vector<uint8_t> dxt(4096);
    for (size_t i = 0; i < dxt.size(); ++i) dxt[i] = (uint8_t)(i * 7 + 3);

    // --- 用例 1：无压缩 DXT5（0xAE） ---
    {
        std::vector<uint8_t> frame;
        appendHeader(frame, 0xAE, (uint32_t)dxt.size());
        frame.insert(frame.end(), dxt.begin(), dxt.end());
        hap::DecodeResult r = hap::decode(frame.data(), frame.size());
        check("uncompressed DXT5", r.ok && r.fmt == hap::PixFmt::DXT5 && same(r.data, dxt));
    }

    // --- 用例 2：整体 snappy DXT5（0xBE） ---
    {
        std::string comp;
        snappy::Compress((const char*)dxt.data(), dxt.size(), &comp);
        std::vector<uint8_t> frame;
        appendHeader(frame, 0xBE, (uint32_t)comp.size());
        frame.insert(frame.end(), comp.begin(), comp.end());
        hap::DecodeResult r = hap::decode(frame.data(), frame.size());
        check("snappy DXT5", r.ok && same(r.data, dxt));
    }

    // --- 用例 3：整体 snappy DXT1（0xBB） ---
    {
        std::vector<uint8_t> dxt1(2048, 0x5A);
        std::string comp;
        snappy::Compress((const char*)dxt1.data(), dxt1.size(), &comp);
        std::vector<uint8_t> frame;
        appendHeader(frame, 0xBB, (uint32_t)comp.size());
        frame.insert(frame.end(), comp.begin(), comp.end());
        hap::DecodeResult r = hap::decode(frame.data(), frame.size());
        check("snappy DXT1", r.ok && r.fmt == hap::PixFmt::DXT1 && same(r.data, dxt1));
    }

    // --- 用例 4：多 chunk（0xCE），3 个 chunk：snappy+原文+snappy，无 offset 表 ---
    {
        std::vector<uint8_t> c1raw(dxt.begin(), dxt.begin() + 1500);
        std::vector<uint8_t> c2raw(dxt.begin() + 1500, dxt.begin() + 3000);
        std::vector<uint8_t> c3raw(dxt.begin() + 3000, dxt.end());
        std::string c1, c3;
        snappy::Compress((const char*)c1raw.data(), c1raw.size(), &c1);
        snappy::Compress((const char*)c3raw.data(), c3raw.size(), &c3);

        // instructions 容器：0x02 压缩器表 + 0x03 尺寸表
        std::vector<uint8_t> container;
        appendHeader(container, 0x02, 3);
        container.push_back(0x0B); container.push_back(0x0A); container.push_back(0x0B);
        appendHeader(container, 0x03, 12);
        for (uint32_t s : {(uint32_t)c1.size(), (uint32_t)c2raw.size(), (uint32_t)c3.size()}) {
            container.push_back(s & 0xFF); container.push_back((s >> 8) & 0xFF);
            container.push_back((s >> 16) & 0xFF); container.push_back((s >> 24) & 0xFF);
        }

        std::vector<uint8_t> sectionData;
        appendHeader(sectionData, 0x01, (uint32_t)container.size());
        sectionData.insert(sectionData.end(), container.begin(), container.end());
        sectionData.insert(sectionData.end(), c1.begin(), c1.end());
        sectionData.insert(sectionData.end(), c2raw.begin(), c2raw.end());
        sectionData.insert(sectionData.end(), c3.begin(), c3.end());

        std::vector<uint8_t> frame;
        appendHeader(frame, 0xCE, (uint32_t)sectionData.size());
        frame.insert(frame.end(), sectionData.begin(), sectionData.end());

        hap::DecodeResult r = hap::decode(frame.data(), frame.size());
        check("multi-chunk DXT5", r.ok && same(r.data, dxt));
    }

    // --- 用例 5：不支持的 BC7（0xBC）应报错 ---
    {
        std::vector<uint8_t> frame;
        appendHeader(frame, 0xBC, 4);
        frame.insert(frame.end(), 4, 0);
        hap::DecodeResult r = hap::decode(frame.data(), frame.size());
        check("reject BC7", !r.ok && !r.err.empty());
    }

    // --- 用例 6：多图组合帧 0x0D（Hap Q Alpha = snappy YCoCg + 无压缩 RGTC1） ---
    {
        std::vector<uint8_t> colorRaw(2048), alphaRaw(512);
        for (size_t i = 0; i < colorRaw.size(); ++i) colorRaw[i] = (uint8_t)(i * 5 + 1);
        for (size_t i = 0; i < alphaRaw.size(); ++i) alphaRaw[i] = (uint8_t)(255 - i % 251);

        std::string colorComp;
        snappy::Compress((const char*)colorRaw.data(), colorRaw.size(), &colorComp);

        std::vector<uint8_t> body;
        appendHeader(body, 0xBF, (uint32_t)colorComp.size());   // snappy YCoCg DXT5
        body.insert(body.end(), colorComp.begin(), colorComp.end());
        appendHeader(body, 0xA1, (uint32_t)alphaRaw.size());    // 无压缩 RGTC1
        body.insert(body.end(), alphaRaw.begin(), alphaRaw.end());

        std::vector<uint8_t> frame;
        appendHeader(frame, 0x0D, (uint32_t)body.size());
        frame.insert(frame.end(), body.begin(), body.end());

        std::vector<uint8_t> out, out2;
        hap::PixFmt fmt; std::string err;
        bool ok = hap::decodeInto(frame.data(), frame.size(), out, fmt, err, &out2);
        check("multi-image Hap Q Alpha", ok && fmt == hap::PixFmt::YCoCg_DXT5
              && same(out, colorRaw) && same(out2, alphaRaw));
    }

    std::printf(g_fail ? "=== 有失败 ===\n" : "=== 全部通过 ===\n");
    return g_fail ? 1 : 0;
}
