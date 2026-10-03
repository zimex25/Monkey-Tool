// nfsnl_audio.cpp - Wwise banks and sounds
//
// What the game actually ships, checked against the files themselves:
//
//  .bnk  a Wwise SoundBank: BKHD / DIDX / DATA / HIRC. DIDX is a flat table
//        of (id, offset, size) into DATA, so splitting a bank into the files
//        it contains is exact - no guessing anywhere.
//
//  .wem  RIFF/WAVE. Every sound in No Limits uses format 0xFFFF, Wwise
//        Vorbis, and its setup header is the "stripped" kind: the codebooks
//        are not in the file, only ten-bit indices into a codebook library
//        that ships with the Wwise encoder. That library is data this tool
//        does not have and cannot derive, so those streams are handed to
//        vgmstream-cli.exe when the user drops it next to MonkeyTool.exe.
//        PCM, float and IMA ADPCM sounds are decoded here directly.
//
//  Gnsu20  the engine sounds: a granular synthesis table (RPM range, grain
//        offsets) the game plays back by picking grains to match engine
//        speed, over one run of EA-XAS audio. gnsuToWav decodes that run, so
//        every grain is heard in order, idle to redline.
#include "nfsnl.h"
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace nfsnl {

namespace {

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void put16(Bytes& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}
void putTag(Bytes& b, const char* t) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)t[i]); }

// IMA ADPCM, as Wwise stores it: interleaved per-channel blocks, each block
// starting with a 4-byte (predictor, index, pad) preamble per channel.
const int kImaStep[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
    724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,
    3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767
};
const int kImaIndex[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

inline int16_t imaStep(uint8_t nibble, int& predictor, int& index) {
    int step = kImaStep[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    predictor += diff;
    predictor = std::min(32767, std::max(-32768, predictor));
    index += kImaIndex[nibble & 15];
    index = std::min(88, std::max(0, index));
    return (int16_t)predictor;
}

} // namespace

// --------------------------------------------------------------- .wem

bool readWemInfo(const uint8_t* d, size_t len, WemInfo& info) {
    info = WemInfo();
    if (len < 44 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return false;
    size_t i = 12;
    const uint8_t* fmt = nullptr;
    size_t fmtLen = 0;
    while (i + 8 <= len) {
        const uint8_t* tag = d + i;
        uint32_t sz = rd32(d + i + 4);
        if (sz > len - i - 8) sz = (uint32_t)(len - i - 8);
        if (!memcmp(tag, "fmt ", 4)) { fmt = d + i + 8; fmtLen = sz; }
        else if (!memcmp(tag, "data", 4)) { info.dataOffset = i + 8; info.dataSize = sz; }
        i += 8 + sz + (sz & 1);
    }
    if (!fmt || fmtLen < 16) return false;
    info.codec = rd16(fmt);
    info.channels = rd16(fmt + 2);
    info.sampleRate = rd32(fmt + 4);
    info.bitsPerSample = rd16(fmt + 14);
    info.blockAlign = rd16(fmt + 12);
    info.fmtSize = (uint32_t)fmtLen;

    // Wwise Vorbis keeps its own header inside the fmt extension. The layout
    // below is the 0x42-byte form; sample count and the two packet offsets
    // are the parts worth reporting.
    if (info.codec == 0xFFFF && fmtLen >= 0x42 - 8) {
        const uint8_t* vorb = fmt + 0x18;
        size_t avail = fmtLen - 0x18;
        if (fmtLen > 0x18 + 0x2A - 1) {
            info.totalSamples = rd32(vorb);
            info.setupPacketOffset = rd32(vorb + 0x10);
            info.firstAudioPacketOffset = rd32(vorb + 0x14);
            info.blockSize0 = vorb[0x28];
            info.blockSize1 = vorb[0x29];
            // A setup packet that begins with 0x05 "vorbis" carries its own
            // codebooks; anything else refers to the external library.
            size_t so = info.dataOffset + info.setupPacketOffset;
            if (so + 9 < len) {
                const uint8_t* p = d + so + 2;   // skip the packet's size field
                info.inlineSetup = (p[0] == 0x05 && !memcmp(p + 1, "vorbis", 6));
            }
        }
        (void)avail;
    }
    if (info.sampleRate && !info.totalSamples && info.bitsPerSample && info.channels) {
        info.totalSamples = (uint32_t)(info.dataSize * 8 /
                            (info.bitsPerSample * info.channels));
    }
    return info.dataSize > 0;
}

const char* wemCodecName(int codec) {
    switch (codec) {
    case 0x0001: return "PCM";
    case 0x0002: return "IMA ADPCM";
    case 0x0003: return "IEEE float";
    case 0x0011: return "IMA ADPCM";
    case 0x0166: return "XMA2";
    case 0xFFFE: return "PCM (extensible)";
    case 0xFFFF: return "Wwise Vorbis";
    default: return "unknown";
    }
}

Bytes makeWav(const int16_t* samples, size_t frameCount, int channels, int sampleRate) {
    Bytes w;
    size_t bytes = frameCount * channels * 2;
    w.reserve(bytes + 44);
    putTag(w, "RIFF"); put32(w, (uint32_t)(36 + bytes)); putTag(w, "WAVE");
    putTag(w, "fmt "); put32(w, 16);
    put16(w, 1); put16(w, (uint16_t)channels);
    put32(w, (uint32_t)sampleRate);
    put32(w, (uint32_t)(sampleRate * channels * 2));
    put16(w, (uint16_t)(channels * 2)); put16(w, 16);
    putTag(w, "data"); put32(w, (uint32_t)bytes);
    const uint8_t* src = (const uint8_t*)samples;
    w.insert(w.end(), src, src + bytes);
    return w;
}

bool wemToWav(const uint8_t* d, size_t len, Bytes& out, std::string* error) {
    WemInfo info;
    if (!readWemInfo(d, len, info)) {
        if (error) *error = "not a Wwise .wem (no RIFF/WAVE header)";
        return false;
    }
    const uint8_t* pay = d + info.dataOffset;
    size_t paySize = info.dataSize;
    int ch = std::max(1, (int)info.channels);

    if (info.codec == 0x0001 || info.codec == 0xFFFE) {
        if (info.bitsPerSample == 16) {
            std::vector<int16_t> s(paySize / 2);
            memcpy(s.data(), pay, s.size() * 2);
            out = makeWav(s.data(), s.size() / ch, ch, info.sampleRate);
            return true;
        }
        if (info.bitsPerSample == 8) {
            std::vector<int16_t> s(paySize);
            for (size_t i = 0; i < paySize; ++i) s[i] = (int16_t)((pay[i] - 128) << 8);
            out = makeWav(s.data(), s.size() / ch, ch, info.sampleRate);
            return true;
        }
        if (error) *error = "unsupported PCM bit depth";
        return false;
    }

    if (info.codec == 0x0003 && info.bitsPerSample == 32) {
        size_t n = paySize / 4;
        std::vector<int16_t> s(n);
        for (size_t i = 0; i < n; ++i) {
            float f;
            memcpy(&f, pay + i * 4, 4);
            f = std::min(1.0f, std::max(-1.0f, f));
            s[i] = (int16_t)std::lround(f * 32767.0f);
        }
        out = makeWav(s.data(), n / ch, ch, info.sampleRate);
        return true;
    }

    if (info.codec == 0x0002 || info.codec == 0x0011) {
        int align = info.blockAlign ? info.blockAlign : 36 * ch;
        int perBlock = ((align / ch) - 4) * 2 + 1;   // samples each block holds
        if (perBlock <= 1) { if (error) *error = "bad ADPCM block size"; return false; }
        std::vector<int16_t> s;
        s.reserve(paySize * 2);
        std::vector<int> pred(ch), idx(ch);
        for (size_t b = 0; b + (size_t)align <= paySize; b += align) {
            const uint8_t* blk = pay + b;
            for (int c = 0; c < ch; ++c) {
                pred[c] = (int16_t)rd16(blk + c * 4);
                idx[c] = std::min(88, (int)blk[c * 4 + 2]);
            }
            size_t base = s.size();
            s.resize(base + (size_t)perBlock * ch, 0);
            for (int c = 0; c < ch; ++c) s[base + c] = (int16_t)pred[c];
            // the rest of the block is nibbles, four bytes per channel at a time
            size_t nibbleStart = (size_t)ch * 4;
            int written = 1;
            size_t p = nibbleStart;
            while (written < perBlock && p + (size_t)ch * 4 <= (size_t)align) {
                for (int c = 0; c < ch; ++c) {
                    for (int k = 0; k < 4 && written + k * 2 < perBlock + 1; ++k) {
                        uint8_t byte = blk[p + c * 4 + k];
                        int lo = byte & 15, hi = byte >> 4;
                        size_t i0 = base + (size_t)(written + k * 2) * ch + c;
                        size_t i1 = i0 + (size_t)ch;
                        if (i0 < s.size()) s[i0] = imaStep((uint8_t)lo, pred[c], idx[c]);
                        if (i1 < s.size()) s[i1] = imaStep((uint8_t)hi, pred[c], idx[c]);
                    }
                }
                written += 8;
                p += (size_t)ch * 4;
            }
        }
        out = makeWav(s.data(), s.size() / ch, ch, info.sampleRate);
        return true;
    }

    if (info.codec == 0xFFFF) {
        if (error) {
            *error = info.inlineSetup
                ? "Wwise Vorbis with inline codebooks - not decoded yet"
                : "Wwise Vorbis: the codebooks live in the Wwise encoder's library, "
                  "not in the file. Put vgmstream-cli.exe next to MonkeyTool.exe and "
                  "this sound converts to wav automatically.";
        }
        return false;
    }
    if (error) {
        char b[128];
        snprintf(b, sizeof(b), "audio codec 0x%04X (%s) is not decoded",
                 info.codec, wemCodecName(info.codec));
        *error = b;
    }
    return false;
}

// --------------------------------------------------------------- Gnsu20

bool readGnsuInfo(const uint8_t* d, size_t len, GnsuInfo& g) {
    g = GnsuInfo();
    if (len < 40 || memcmp(d, "Gnsu", 4) != 0) return false;
    g.version = std::string((const char*)d + 4, 2);
    memcpy(&g.minRpm, d + 8, 4);
    memcpy(&g.maxRpm, d + 12, 4);
    g.entryCount = rd32(d + 16);      // the rev table's length (50)
    g.grainCount = rd32(d + 20);      // grains, each a start sample
    g.totalSamples = rd32(d + 24);
    g.sampleRate = rd32(d + 28);
    return true;
}

// The grains themselves are one run of EA-XAS (version 0) audio: 19-byte
// frames of 32 mono samples - a 32-bit header holding the first two samples
// (12-bit precision), the predictor and the shift, then 15 bytes of 4-bit
// deltas, high nibble first. The same codec as Burnout Paradise's and
// Hot Pursuit's engine banks. Decoded end to end it is every grain in a row,
// idle to redline, which is how vgmstream plays these files too.
bool gnsuToWav(const uint8_t* d, size_t len, Bytes& out, std::string* error) {
    GnsuInfo g;
    if (!readGnsuInfo(d, len, g)) {
        if (error) *error = "not a Gnsu engine bank";
        return false;
    }
    if (g.sampleRate < 4000 || g.sampleRate > 96000 || g.totalSamples == 0 ||
        g.totalSamples > 100000000u) {
        if (error) *error = "the engine bank's header is out of range";
        return false;
    }
    size_t frames = ((size_t)g.totalSamples + 31) / 32;
    if (frames * 19 > len - 32) {
        if (error) *error = "the engine bank is shorter than its header says";
        return false;
    }
    // the audio runs to the end of the file; the tables come before it
    size_t start = len - frames * 19;
    static const int kCoef[4][2] = { {0, 0}, {240, 0}, {460, -208}, {392, -220} };
    std::vector<int16_t> pcm;
    pcm.reserve(frames * 32);
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* fr = d + start + f * 19;
        uint32_t h = rd32(fr);
        int c1 = kCoef[h & 3][0], c2 = kCoef[h & 3][1];
        int s2 = (int16_t)(uint16_t)(h & 0xFFF0);
        int s1 = (int16_t)(uint16_t)((h >> 16) & 0xFFF0);
        int shift = (h >> 16) & 0x0F;
        pcm.push_back((int16_t)s2);
        pcm.push_back((int16_t)s1);
        for (int j = 0; j < 15; ++j)
            for (int k = 0; k < 2; ++k) {
                int n = k == 0 ? fr[4 + j] >> 4 : fr[4 + j] & 0x0F;
                if (n > 7) n -= 16;
                int v = (n * (1 << 12)) >> shift;
                v += (c1 * s1 + c2 * s2 + 128) >> 8;
                v = std::max(-32768, std::min(32767, v));
                s2 = s1;
                s1 = v;
                pcm.push_back((int16_t)v);
            }
    }
    pcm.resize(g.totalSamples);
    out = makeWav(pcm.data(), pcm.size(), 1, (int)g.sampleRate);
    return true;
}

// --------------------------------------------------------------- .sps
//
// Real Racing 2 (and other EA mobile games of its time) keep sounds as SPS:
// EA's "EAAudioCore" stream in blocks. Each block is a type byte and a
// 24-bit big-endian size:
//   'H' (0x48)  the header: u32 BE codec word - version (4 bits), codec
//               (4 bits), channels - 1 (6 bits), sample rate (18 bits) - then
//               u32 BE: type (2 bits), loop flag (1 bit), sample count (29)
//   'D' (0x44)  data: u32 BE samples in the block, then the frames
//   'E' (0x45)  the end
// Codec 4 is EA-XAS v1: frames of 128 samples per channel, 0x4c bytes -
// four little-endian group headers (the first two samples, the predictor and
// the shift of each 32-sample group), then 15 rows of four bytes, one byte
// per group per row, high nibble first. The predictor is the one EA-XA and
// the Gnsu engine banks use. Codec 2 is plain 16-bit big-endian PCM.
bool spsInfo(const uint8_t* d, size_t len, int* codec, int* channels, int* rate, uint32_t* samples) {
    if (len < 16 || d[0] != 0x48) return false;
    uint32_t hsz = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    if (hsz < 12 || hsz > len) return false;
    uint32_t h1 = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    uint32_t h2 = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
    if (codec) *codec = (int)((h1 >> 24) & 0x0F);
    if (channels) *channels = (int)((h1 >> 18) & 0x3F) + 1;
    if (rate) *rate = (int)(h1 & 0x3FFFF);
    if (samples) *samples = h2 & 0x1FFFFFFF;
    return true;
}

bool spsToWav(const uint8_t* d, size_t len, Bytes& out, std::string* error) {
    int codec = 0, ch = 1, rate = 0;
    uint32_t total = 0;
    if (!spsInfo(d, len, &codec, &ch, &rate, &total)) {
        if (error) *error = "not an EA SPS stream";
        return false;
    }
    if (rate < 1000 || rate > 192000 || ch < 1 || ch > 8) {
        if (error) *error = "the SPS header is out of range";
        return false;
    }
    if (codec != 4 && codec != 2) {
        static const char* names[] = { "none", "reserved", "PCM", "EA-XMA", "EA-XAS", "EALayer3 v1",
                                       "EALayer3 v2", "EALayer3 v2 spike", "GameCube ADPCM", "EA-Speex",
                                       "ATRAC9", "EA-MP3", "EA-Opus", "?", "?", "?" };
        if (error) *error = std::string("SPS codec ") + names[codec & 15] + " - played through vgmstream";
        return false;
    }
    static const int kCoef[4][2] = { {0, 0}, {240, 0}, {460, -208}, {392, -220} };
    std::vector<std::vector<int16_t>> pcm(ch);
    size_t o = 0;
    while (o + 4 <= len) {
        uint8_t type = d[o];
        uint32_t sz = ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
        if (sz < 4 || o + sz > len) break;
        if (type == 0x45) break;
        if (type == 0x44 && sz >= 8) {
            uint32_t n = ((uint32_t)d[o + 4] << 24) | ((uint32_t)d[o + 5] << 16) |
                         ((uint32_t)d[o + 6] << 8) | d[o + 7];
            const uint8_t* p = d + o + 8;
            const uint8_t* end = d + o + sz;
            if (codec == 2) {
                for (uint32_t i = 0; i < n; ++i)
                    for (int c = 0; c < ch; ++c) {
                        if (p + 2 > end) break;
                        pcm[c].push_back((int16_t)((p[0] << 8) | p[1]));
                        p += 2;
                    }
            } else {
                for (uint32_t done = 0; done < n; done += 128) {
                    for (int c = 0; c < ch; ++c) {
                        const uint8_t* fr = p + c * 0x4c;
                        if (fr + 0x4c > end) break;
                        for (int g = 0; g < 4; ++g) {
                            uint32_t h = (uint32_t)fr[g * 4] | ((uint32_t)fr[g * 4 + 1] << 8) |
                                         ((uint32_t)fr[g * 4 + 2] << 16) | ((uint32_t)fr[g * 4 + 3] << 24);
                            int c1 = kCoef[h & 3][0], c2 = kCoef[h & 3][1];
                            int s2 = (int16_t)(uint16_t)(h & 0xFFF0);
                            int s1 = (int16_t)(uint16_t)((h >> 16) & 0xFFF0);
                            int shift = (h >> 16) & 0x0F;
                            pcm[c].push_back((int16_t)s2);
                            pcm[c].push_back((int16_t)s1);
                            for (int r = 0; r < 15; ++r) {
                                uint8_t b = fr[0x10 + g + r * 4];
                                for (int k = 0; k < 2; ++k) {
                                    int nib = k == 0 ? b >> 4 : b & 0x0F;
                                    if (nib > 7) nib -= 16;
                                    int v = (nib * (1 << 12)) >> shift;
                                    v += (c1 * s1 + c2 * s2 + 128) >> 8;
                                    v = std::max(-32768, std::min(32767, v));
                                    s2 = s1;
                                    s1 = v;
                                    pcm[c].push_back((int16_t)v);
                                }
                            }
                        }
                    }
                    p += 0x4c * ch;
                }
            }
        }
        o += sz;
    }
    size_t frames = pcm[0].size();
    for (auto& v : pcm) frames = std::min(frames, v.size());
    if (total && total < frames) frames = total;
    if (!frames) {
        if (error) *error = "the SPS stream holds no audio";
        return false;
    }
    std::vector<int16_t> inter(frames * ch);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) inter[i * ch + c] = pcm[c][i];
    out = makeWav(inter.data(), frames, ch, rate);
    return true;
}

// --------------------------------------------------------------- .bnk

bool readBank(const uint8_t* d, size_t len, Bank& bank) {
    bank = Bank();
    if (len < 12 || memcmp(d, "BKHD", 4) != 0) return false;
    size_t i = 0;
    size_t dataOff = 0, dataLen = 0;
    std::vector<BankEntry> index;
    while (i + 8 <= len) {
        const uint8_t* tag = d + i;
        uint32_t sz = rd32(d + i + 4);
        if (sz > len - i - 8) sz = (uint32_t)(len - i - 8);
        const uint8_t* body = d + i + 8;
        if (!memcmp(tag, "BKHD", 4) && sz >= 8) {
            bank.version = rd32(body);
            bank.id = rd32(body + 4);
        } else if (!memcmp(tag, "DIDX", 4)) {
            for (uint32_t k = 0; k + 12 <= sz; k += 12) {
                BankEntry e;
                e.id = rd32(body + k);
                e.offset = rd32(body + k + 4);
                e.size = rd32(body + k + 8);
                index.push_back(e);
            }
        } else if (!memcmp(tag, "DATA", 4)) {
            dataOff = i + 8;
            dataLen = sz;
        } else if (!memcmp(tag, "HIRC", 4) && sz >= 4) {
            bank.hircObjects = rd32(body);
            // The playback objects name the media they play: a Sound (2)
            // has one source, a Music Track (11) a list. Sources that are
            // not in DIDX live outside the bank, as loose <id>.wem files -
            // which is how a loose file can be traced to the bank that
            // plays it.
            size_t q = 4;
            for (uint32_t k = 0; k < bank.hircObjects && q + 9 <= sz; ++k) {
                uint8_t type = body[q];
                uint32_t osz = rd32(body + q + 1);
                if (osz < 4 || q + 5 + (size_t)osz > sz) break;
                const uint8_t* pl = body + q + 9;          // after the object id
                size_t plen = osz - 4;
                if (type == 2 && plen >= 9) {
                    bank.sources.push_back(rd32(pl + 5));  // plugin u32, stream type u8, source id
                } else if (type == 11 && plen >= 5) {
                    uint32_t n = rd32(pl + 1);             // flags u8, source count
                    for (uint32_t j = 0; j < n && j < 256 && 5 + (size_t)j * 14 + 9 <= plen; ++j)
                        bank.sources.push_back(rd32(pl + 5 + (size_t)j * 14 + 5));
                }
                q += 5 + (size_t)osz;
            }
        }
        bank.sections.push_back(std::string((const char*)tag, 4));
        i += 8 + sz;
    }
    for (auto& e : index) {
        if (e.offset + e.size > dataLen) continue;
        e.offset += dataOff;                    // make it absolute in the bank
        const uint8_t* p = d + e.offset;
        if (e.size >= 12 && !memcmp(p, "RIFF", 4)) {
            e.kind = "wem";
            WemInfo wi;
            if (readWemInfo(p, e.size, wi)) {
                e.codec = wi.codec;
                e.channels = wi.channels;
                e.sampleRate = wi.sampleRate;
                e.samples = wi.totalSamples;
            }
        } else if (e.size >= 8 && !memcmp(p, "Gnsu", 4)) {
            e.kind = "gnsu";
            GnsuInfo g;
            if (readGnsuInfo(p, e.size, g)) {
                e.sampleRate = g.sampleRate;
                e.samples = g.totalSamples;
                e.minRpm = g.minRpm;
                e.maxRpm = g.maxRpm;
            }
        } else {
            e.kind = "bin";
        }
        bank.entries.push_back(e);
    }
    return !bank.sections.empty();
}

std::string describeBank(const Bank& bank) {
    std::string s;
    char b[512];
    snprintf(b, sizeof(b), "Wwise SoundBank\r\nbank id %u, version %u\r\nsections:",
             bank.id, bank.version);
    s += b;
    for (const auto& sec : bank.sections) { s += " "; s += sec; }
    snprintf(b, sizeof(b), "\r\n%u sound(s) embedded", (unsigned)bank.entries.size());
    s += b;
    if (bank.hircObjects) {
        snprintf(b, sizeof(b), ", %u playback object(s)", bank.hircObjects);
        s += b;
    }
    for (const auto& e : bank.entries) {
        if (e.kind == "gnsu") {
            snprintf(b, sizeof(b),
                     "\r\n  %u  granular engine sound, %u Hz, %.0f-%.0f rpm, %u bytes",
                     e.id, e.sampleRate, e.minRpm, e.maxRpm, (unsigned)e.size);
        } else if (e.kind == "wem") {
            snprintf(b, sizeof(b), "\r\n  %u  %s, %u ch, %u Hz, %u bytes",
                     e.id, wemCodecName(e.codec), e.channels, e.sampleRate,
                     (unsigned)e.size);
        } else {
            snprintf(b, sizeof(b), "\r\n  %u  unrecognised, %u bytes", e.id, (unsigned)e.size);
        }
        s += b;
    }
    return s;
}

// ---- FMOD ----
//
// FSB5: "FSB5", version, sample count, sample-header size, name-table size,
// data size, mode, then (version 1) 36 more bytes to 0x3C, the sample
// headers, and the name table: one u32 offset per sample (from the table's
// start), then the names, each ending in a zero.
std::vector<std::string> fsb5Names(const uint8_t* d, size_t len) {
    std::vector<std::string> out;
    if (len < 0x3C || memcmp(d, "FSB5", 4) != 0) return out;
    uint32_t ver, count, shdr, ntab;
    memcpy(&ver, d + 4, 4);
    memcpy(&count, d + 8, 4);
    memcpy(&shdr, d + 12, 4);
    memcpy(&ntab, d + 16, 4);
    size_t base = ver == 0 ? 0x40 : 0x3C;
    if (count == 0 || count > 100000) return out;
    size_t nt = base + shdr;
    if (ntab == 0 || nt + (size_t)count * 4 > len) {
        for (uint32_t i = 0; i < count; ++i) out.push_back("sound " + std::to_string(i + 1));
        return out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t o;
        memcpy(&o, d + nt + (size_t)i * 4, 4);
        std::string n;
        for (size_t k = nt + o; k < len && k < nt + ntab && d[k]; ++k) n.push_back((char)d[k]);
        out.push_back(n.empty() ? "sound " + std::to_string(i + 1) : n);
    }
    return out;
}

// The readable strings of an .fev (RIFF "FEV "): the project, its event
// groups and events, and the .wav files they play.
std::vector<std::string> fevStrings(const uint8_t* d, size_t len) {
    std::vector<std::string> out;
    if (len < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "FEV ", 4) != 0) return out;
    std::string cur;
    for (size_t i = 12; i <= len; ++i) {
        uint8_t c = i < len ? d[i] : 0;
        if (c >= 0x20 && c < 0x7F) { cur.push_back((char)c); continue; }
        if (c == 0 && cur.size() >= 3) {
            int letters = 0;
            bool clean = true;
            for (char ch : cur) {
                if (isalpha((unsigned char)ch)) ++letters;
                else if (!isdigit((unsigned char)ch) && !strchr("_ ./-", ch)) clean = false;
            }
            if (clean && letters >= 3 && (out.empty() || out.back() != cur)) out.push_back(cur);
        }
        cur.clear();
    }
    return out;
}

} // namespace nfsnl
