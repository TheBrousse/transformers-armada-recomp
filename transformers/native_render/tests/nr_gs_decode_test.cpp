// nr_gs_decode_test: nr_core's GS address functions (native_render/gs_decode.h) against the runtime's own (runtime/gs/ps2_gs_psm*.h),
// every block / width / x / y class: they must agree on every address. Built by the CMake target nr_gs_decode_test (not part of ALL).
#include "gs_decode.h"

#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <cstdint>
#include <cstdio>

int main()
{
    uint64_t checked = 0, bad = 0;
    const uint32_t blocks[] = {0u, 1u, 7u, 31u, 32u, 33u, 64u, 96u, 255u, 256u, 500u, 1024u, 4095u, 8191u, 16383u};
    for (const uint32_t block : blocks)
        for (uint32_t width = 0; width <= 20; ++width)
            for (uint32_t y = 0; y < 300; ++y)
                for (uint32_t x = 0; x < 300; ++x)
                {
                    const uint32_t a[] = {GSPSMCT32::addrPSMCT32(block, width, x, y), GSPSMCT16::addrPSMCT16(block, width, x, y),
                                          GSPSMCT16::addrPSMCT16S(block, width, x, y), GSPSMCT16::addrPSMZ16(block, width, x, y),
                                          GSPSMCT16::addrPSMZ16S(block, width, x, y), GSPSMT8::addrPSMT8(block, width, x, y),
                                          GSPSMT4::addrPSMT4(block, width, x, y)};
                    const uint32_t b[] = {nr_core::gs::gspsmct32::addrPSMCT32(block, width, x, y),
                                          nr_core::gs::gspsmct16::addrPSMCT16(block, width, x, y),
                                          nr_core::gs::gspsmct16::addrPSMCT16S(block, width, x, y),
                                          nr_core::gs::gspsmct16::addrPSMZ16(block, width, x, y),
                                          nr_core::gs::gspsmct16::addrPSMZ16S(block, width, x, y),
                                          nr_core::gs::gspsmt8::addrPSMT8(block, width, x, y),
                                          nr_core::gs::gspsmt4::addrPSMT4(block, width, x, y)};
                    for (int k = 0; k < 7; ++k)
                    {
                        ++checked;
                        if (a[k] != b[k])
                            ++bad;
                    }
                }
    std::printf("nr_gs_decode_test: %llu addresses compared, %llu differ\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad));
    return bad ? 1 : 0;
}
