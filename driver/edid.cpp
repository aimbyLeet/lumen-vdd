// EDID 1.4 base block for Lumen VDD monitors.

#include "driver.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lumen_vdd {

namespace {

// Manufacturer "LMN": three 5-bit letters (A = 1), big-endian: (12 << 10) | (13 << 5) | 14.
constexpr uint8_t kManufacturer[2] = {0x31, 0xAE};
constexpr uint16_t kProductBase = 0x4C00;
constexpr uint32_t kSerialBase = 0x4C560000;
constexpr char kName[] = "LumenVDA";  // the monitor's name, as Parsec's is ParsecVDA (owner, 2026-11-12)

struct Timing {
    uint32_t width, height;
    uint32_t hblank, hfp, hsync;
    uint32_t vblank, vfp, vsync;
    uint64_t pixel_clock_hz;
};

// CVT reduced blanking (v1) style timing: 160 px horizontal blanking, at least 460 us of vertical blanking.
// Exact CVT rounding does not matter: Windows takes the modes from the driver, the EDID only has to be valid.
Timing ReducedBlanking(const LUMEN_VDD_MODE& mode) {
    Timing t = {};
    t.width = mode.width;
    t.height = mode.height;
    t.hblank = 160;
    t.hfp = 48;
    t.hsync = 32;
    t.vfp = 3;
    t.vsync = 6;
    const double line_period = (1.0 / mode.refresh_hz - 460e-6) / mode.height;
    const uint32_t min_vblank = t.vfp + t.vsync + 6;
    t.vblank = static_cast<uint32_t>(std::ceil(460e-6 / line_period));
    if (t.vblank < min_vblank) {
        t.vblank = min_vblank;
    }
    t.pixel_clock_hz =
        static_cast<uint64_t>(t.width + t.hblank) * (t.height + t.vblank) * static_cast<uint64_t>(mode.refresh_hz);
    return t;
}

void WriteDetailedTiming(uint8_t* d, const Timing& t) {
    const uint32_t clock = static_cast<uint32_t>(t.pixel_clock_hz / 10000);
    d[0] = clock & 0xFF;
    d[1] = (clock >> 8) & 0xFF;
    d[2] = t.width & 0xFF;
    d[3] = t.hblank & 0xFF;
    d[4] = static_cast<uint8_t>(((t.width >> 8) & 0xF) << 4 | ((t.hblank >> 8) & 0xF));
    d[5] = t.height & 0xFF;
    d[6] = t.vblank & 0xFF;
    d[7] = static_cast<uint8_t>(((t.height >> 8) & 0xF) << 4 | ((t.vblank >> 8) & 0xF));
    d[8] = t.hfp & 0xFF;
    d[9] = t.hsync & 0xFF;
    d[10] = static_cast<uint8_t>((t.vfp & 0xF) << 4 | (t.vsync & 0xF));
    d[11] = static_cast<uint8_t>(((t.hfp >> 8) & 3) << 6 | ((t.hsync >> 8) & 3) << 4 | ((t.vfp >> 4) & 3) << 2 |
                                 ((t.vsync >> 4) & 3));
    d[12] = 0;  // image size unknown (as in bytes 21-22)
    d[13] = 0;
    d[14] = 0;
    d[15] = 0;
    d[16] = 0;
    d[17] = 0x1A;  // digital separate sync, hsync +, vsync - (reduced blanking)
}

// Display descriptor with a text payload (name 0xFC, serial 0xFF): up to 13 chars, 0x0A-terminated, space-padded.
void WriteTextDescriptor(uint8_t* d, uint8_t tag, const char* text) {
    memset(d, 0, 18);
    d[3] = tag;
    size_t i = 0;
    for (; i < 13 && text[i] != '\0'; i++) {
        d[5 + i] = static_cast<uint8_t>(text[i]);
    }
    if (i < 13) {
        d[5 + i++] = 0x0A;
    }
    for (; i < 13; i++) {
        d[5 + i] = 0x20;
    }
}

void WriteChromaticity(uint8_t* e) {
    // sRGB primaries and D65 white point, 10-bit fractions
    auto q = [](double v) { return static_cast<uint32_t>(v * 1024 + 0.5); };
    const uint32_t rx = q(0.640), ry = q(0.330), gx = q(0.300), gy = q(0.600);
    const uint32_t bx = q(0.150), by = q(0.060), wx = q(0.3127), wy = q(0.3290);
    e[25] = static_cast<uint8_t>((rx & 3) << 6 | (ry & 3) << 4 | (gx & 3) << 2 | (gy & 3));
    e[26] = static_cast<uint8_t>((bx & 3) << 6 | (by & 3) << 4 | (wx & 3) << 2 | (wy & 3));
    e[27] = static_cast<uint8_t>(rx >> 2);
    e[28] = static_cast<uint8_t>(ry >> 2);
    e[29] = static_cast<uint8_t>(gx >> 2);
    e[30] = static_cast<uint8_t>(gy >> 2);
    e[31] = static_cast<uint8_t>(bx >> 2);
    e[32] = static_cast<uint8_t>(by >> 2);
    e[33] = static_cast<uint8_t>(wx >> 2);
    e[34] = static_cast<uint8_t>(wy >> 2);
}

}  // namespace

std::vector<uint8_t> MakeEdid(uint32_t id) {
    std::vector<uint8_t> e(kEdidBlock);
    uint8_t* p = e.data();

    static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memcpy(p, kHeader, sizeof(kHeader));
    p[8] = kManufacturer[0];
    p[9] = kManufacturer[1];
    const uint16_t product = static_cast<uint16_t>(kProductBase + id);
    p[10] = product & 0xFF;
    p[11] = product >> 8;
    const uint32_t serial = kSerialBase + id;
    for (int i = 0; i < 4; i++) {
        p[12 + i] = (serial >> (8 * i)) & 0xFF;
    }
    p[16] = 0;              // week of manufacture: unknown
    p[17] = 2026 - 1990;    // year of manufacture
    p[18] = 1;              // EDID 1.4
    p[19] = 4;
    p[20] = 0xA5;           // digital input, 8 bits per color, DisplayPort

    // Fixed preferred timing: the EDID must not depend on the modes (see MakeEdid). Windows takes the preferred mode
    // from the driver's mode list.
    const Timing timing = ReducedBlanking(LUMEN_VDD_MODE{1920, 1080, 60});
    // Physical size unknown: Windows has no DPI to derive a scale from and defaults to 100 %; Lumen Host sets the
    // real scale.
    p[21] = 0;
    p[22] = 0;
    p[23] = 120;            // gamma 2.2: (2.2 - 1) * 100
    p[24] = 0x06;           // RGB 4:4:4, sRGB default color space, preferred timing is native
    WriteChromaticity(p);
    // 35-37 established timings: none
    for (int i = 38; i < 54; i++) {
        p[i] = 0x01;        // standard timings: unused
    }

    WriteDetailedTiming(p + 54, timing);
    char serial_text[14];
    sprintf_s(serial_text, "LVDD-%u", id);
    WriteTextDescriptor(p + 72, 0xFF, serial_text);
    WriteTextDescriptor(p + 90, 0xFC, kName);
    memset(p + 108, 0, 18);
    p[108 + 3] = 0x10;      // dummy descriptor

    p[126] = 0;             // no extension blocks
    uint8_t sum = 0;
    for (size_t i = 0; i < 127; i++) {
        sum = static_cast<uint8_t>(sum + p[i]);
    }
    p[127] = static_cast<uint8_t>(0x100 - sum);
    return e;
}

bool IsValidEdid(const uint8_t* edid, size_t size) {
    static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (edid == nullptr || size < kEdidBlock || size > LUMEN_VDD_MAX_EDID || size % kEdidBlock != 0 ||
        memcmp(edid, kHeader, sizeof(kHeader)) != 0) {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < kEdidBlock; i++) {
        sum = static_cast<uint8_t>(sum + edid[i]);
    }
    return sum == 0;
}

}  // namespace lumen_vdd
