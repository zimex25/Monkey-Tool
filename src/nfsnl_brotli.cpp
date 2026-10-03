// Brotli decoding (RFC 7932) for NFS Edge's FMOBB archives, whose packed
// files are Brotli streams. The decoder is Google's reference code (MIT
// licence, see src/brotli/LICENSE), compiled here as one unit so the build
// needs no extra steps and no DLL.
#include "nfsnl.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include "brotli/common/constants.c"
#include "brotli/common/context.c"
#include "brotli/common/dictionary.c"
#include "brotli/common/platform.c"
#include "brotli/common/shared_dictionary.c"
#include "brotli/common/transform.c"
#include "brotli/dec/bit_reader.c"
#include "brotli/dec/huffman.c"
#include "brotli/dec/prefix.c"
#include "brotli/dec/state.c"
#include "brotli/dec/static_init.c"
#include "brotli/dec/decode.c"

namespace nfsnl {

bool brotliDecompress(const uint8_t* src, size_t n, size_t expected, Bytes& out) {
    out.assign(expected, 0);
    size_t got = expected;
    if (BrotliDecoderDecompress(n, src, &got, out.data()) != BROTLI_DECODER_RESULT_SUCCESS)
        return false;
    out.resize(got);
    return true;
}

}
