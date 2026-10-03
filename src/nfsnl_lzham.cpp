// nfsnl_lzham.cpp - the LZHAM decompressor built into the program
//
// No Limits' .m3g models and some of its cabinets (and No Limits VR's) are
// LZHAM streams. Up to 1.1.5 the decoder was a DLL beside the program, built
// separately - and people kept ending up with one that would not load (the
// wrong build, or one needing a Visual C++ runtime they did not have). The
// decompressor is public-domain code (src/lzham/, Rich Geldreich's LZHAM), so
// it is now simply compiled in. A DLL picked with File > Locate still wins.
#include "lzham/lzham.h"
#include "lzham/lzham_decomp.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nfsnl {

int lzhamBuiltinDecompress(uint32_t dictLog2, uint32_t updateRate, uint32_t flags,
                           uint8_t* dst, size_t* dstLen, const uint8_t* src, size_t srcLen,
                           uint32_t* adler) {
    lzham_decompress_params p;
    memset(&p, 0, sizeof(p));
    p.m_struct_size = sizeof(p);
    p.m_dict_size_log2 = dictLog2;
    p.m_table_update_rate = updateRate;
    p.m_decompress_flags = flags;
    lzham_uint32 sum = 0;
    lzham_decompress_status_t st = lzham::lzham_lib_decompress_memory(&p, dst, dstLen, src, srcLen, &sum);
    if (adler) *adler = sum;
    return (int)st;
}

} // namespace nfsnl
