// LdEncoder — writes MoTeC-compatible binary .ld files + XML .ldx beacon companions.
// The layout is the exact inverse of DecodeMotecLd() in MotecToCSV.cpp:
//   - U32 @8            = absolute address of first channel block (chain head)
//   - driver name       NUL-terminated, offset 158, max 64 chars
//   - vehicle name      NUL-terminated, offset 222, max 128 chars
//   - track name        NUL-terminated, offset 350, max 128 chars
//   - per block: nextBlock U32 @+4, dataStart U32 @+8, count U32 @+12, type U16 @+18 (7 = float),
//     entrySize U16 @+20 (4), frequency I16 @+22 (>0), decimalShift I16 @+30 (0 for floats)
//   - name NUL-terminated [base+32, base+64); unit NUL-terminated [base+72, base+80)
// All channels are written as type 7 / entrySize 4 (raw IEEE float32 LE), which the decoder
// reads with no scaling — the simplest path it fully supports.

#include "LdEncoder.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#   include <direct.h>
#endif

namespace {

const size_t kMinFileSize = 1636;      // decoder rejects files smaller than this
const size_t kBlockAreaStart = 512;    // first block address (after header text fields, which end at 478)
const size_t kBlockStride = 96;        // per-block slot: content ends at +80, stride keeps slots disjoint

void putU32(std::vector<unsigned char>& buf, size_t offset, unsigned long value) {
    for (int i = 0; i < 4 && offset + static_cast<size_t>(i) < buf.size(); ++i)
        buf[offset + static_cast<size_t>(i)] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
}

void putU16(std::vector<unsigned char>& buf, size_t offset, unsigned short value) {
    for (int i = 0; i < 2 && offset + static_cast<size_t>(i) < buf.size(); ++i)
        buf[offset + static_cast<size_t>(i)] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
}

void writeTextField(std::vector<unsigned char>& buf, size_t offset, const std::string& text, size_t maxChars) {
    // NUL-terminated field: at most maxChars characters + terminating zero.
    for (size_t i = 0; i < maxChars && i < text.size() && offset + i < buf.size(); ++i)
        buf[offset + i] = static_cast<unsigned char>(text[i]);
}

void writeFloat32(std::vector<unsigned char>& buf, size_t offset, float value) {
    if (!std::isfinite(static_cast<double>(value)))
        value = 0.0f;   // keep the file clean for i2 import
    unsigned int bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(buf, offset, static_cast<unsigned long>(bits));
}

// Builds the complete .ld image in memory. Returns false if no channel is usable.
bool buildLdBuffer(const LdExportOptions& opts, const std::vector<LdChannelData>& channelsIn,
                   std::vector<unsigned char>& out) {
    // Keep only valid channels (non-empty name + samples).
    std::vector<const LdChannelData*> chans;
    for (size_t i = 0; i < channelsIn.size(); ++i)
        if (channelsIn[i].valid())
            chans.push_back(&channelsIn[i]);
    const size_t nBlocks = chans.size();
    if (nBlocks == 0)
        return false;

    // Data area starts right after the block chain.
    size_t dataAreaStart = kBlockAreaStart + nBlocks * kBlockStride;   // already a multiple of 4
    std::vector<size_t> channelDataOffset(nBlocks);
    for (size_t i = 0; i < nBlocks; ++i) {
        if (i > 0)
            dataAreaStart += chans[i - 1]->values.size() * sizeof(float);
        channelDataOffset[i] = dataAreaStart;
    }

    size_t totalSize = dataAreaStart + chans[nBlocks - 1]->values.size() * sizeof(float);
    if (totalSize < kMinFileSize)
        totalSize = kMinFileSize;   // zero-padded tail satisfies the decoder's minimum-size check

    out.assign(totalSize, 0);

    putU32(out, 8, static_cast<unsigned long>(kBlockAreaStart));       // chain head -> first block
    writeTextField(out, 158, opts.driverName, 64);
    writeTextField(out, 222, opts.vehicleName, 128);
    writeTextField(out, 350, opts.trackName, 128);

    for (size_t i = 0; i < nBlocks; ++i) {
        const size_t base = kBlockAreaStart + i * kBlockStride;
        putU32(out, base + 4, static_cast<unsigned long>(i + 1 < nBlocks ? base + kBlockStride : 0)); // nextBlock (0 ends chain)
        putU32(out, base + 8, static_cast<unsigned long>(channelDataOffset[i]));                       // dataStart
        putU32(out, base + 12, static_cast<unsigned long>(chans[i]->values.size()));                   // count
        putU16(out, base + 18, 7);      // type: float
        putU16(out, base + 20, 4);      // entrySize
        int freq = chans[i]->frequencyHz;
        if (freq <= 0) freq = 60;       // decoder skips blocks with frequency <= 0
        if (freq > 32767) freq = 32767;
        putU16(out, base + 22, static_cast<unsigned short>(freq));   // I16 in range here
        putU16(out, base + 30, 0);      // decimalShift (ignored for type 7)

        writeTextField(out, base + 32, chans[i]->name, 32);           // name: max 31 chars + NUL
        writeTextField(out, base + 72, chans[i]->unit, 8);            // unit: max 7 chars + NUL

        for (size_t s = 0; s < chans[i]->values.size(); ++s)
            writeFloat32(out, channelDataOffset[i] + s * sizeof(float), chans[i]->values[s]);
    }
    return true;
}

bool writeFileBytes(const std::string& path, const unsigned char* bytes, size_t count) {
#ifdef _WIN32
    FILE* fp = NULL;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp)
        return false;
#else
    FILE* fp = std::fopen(path.c_str(), "wb");
#endif
    if (!fp)
        return false;
    const size_t written = count ? std::fwrite(bytes, 1, count, fp) : 0;
    std::fclose(fp);
    return written == count;
}

} // namespace

bool WriteLdFile(const std::string& path, const LdExportOptions& opts,
                 const std::vector<LdChannelData>& channels) {
    if (channels.empty())
        return false;
    std::vector<unsigned char> buffer;
    if (!buildLdBuffer(opts, channels, buffer))
        return false;
    return writeFileBytes(path, &buffer[0], buffer.size());
}

void WriteLdxBeacons(const std::string& ldxPath, const std::vector<int64_t>& beaconTimesUs) {
#ifdef _WIN32
    FILE* fp = NULL;
    if (fopen_s(&fp, ldxPath.c_str(), "wb") != 0 || !fp)
        return;
#else
    FILE* fp = std::fopen(ldxPath.c_str(), "wb");
#endif
    if (!fp)
        return;

    // The decoder scans for literal `Time="` then strtod()s the value (microseconds).
    const char header[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<MoTeC_LDX>\n  <Beacons>\n";
    std::fwrite(header, 1, sizeof(header) - 1, fp);

    for (size_t i = 0; i < beaconTimesUs.size(); ++i) {
        char line[64];
#ifdef _WIN32
        sprintf_s(line, sizeof(line), "<Beacon Time=\"%I64d\" />\n", static_cast<int64_t>(beaconTimesUs[i]));
#else
        snprintf(line, sizeof(line), "<Beacon Time=\"%lld\" />\n", static_cast<long long>(beaconTimesUs[i]));
#endif
        std::fwrite(line, 1, strlen(line), fp);
    }

    const char footer[] = "  </Beacons>\n</MoTeC_LDX>\n";
    std::fwrite(footer, 1, sizeof(footer) - 1, fp);
    std::fclose(fp);
}

void EnsurePathExists(const std::string& dirPath) {
#ifdef _WIN32
    if (dirPath.empty())
        return;
    // Create each intermediate directory segment (_mkdir fails on existing dirs — that's fine).
    for (size_t i = 1; i < dirPath.size(); ++i) {
        if (dirPath[i] == '\\' || dirPath[i] == '/') {
            std::string sub = dirPath.substr(0, i);
            _mkdir(sub.c_str());   // ignore result: existing directories are OK
        }
    }
    _mkdir(dirPath.c_str());
#endif
}
