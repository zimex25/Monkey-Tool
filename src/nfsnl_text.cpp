// nfsnl_text.cpp - turn an asset into readable text
//
// Every SBIN file carries its own schema: STRU lists the structures, FIEL
// their fields, ENUM the enumerations, and CHDR/CDAT a table of every name
// the file uses. That is what this dump prints - the game's own description
// of the data, not a guess at it. .sbfx (particle effects) and .sba are read
// this way, and so is any other SBIN the game ships.
//
// Files that are not SBIN fall back to a hex and ASCII listing. The .sb data
// files land there for a reason that is stated in the dump: they are
// encrypted, and the key lives in the game binary.
#include "nfsnl.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>

namespace nfsnl {

namespace {

// FIEL type codes, read off the fields the game names unambiguously
// (preRoll/totalTime are floats, cycleCount an int, name a string, and so on).
const char* fieldTypeName(int t) {
    switch (t) {
    case 5:  return "int";
    case 9:  return "bool";
    case 10: return "float";
    case 14: return "vector4";
    case 15: return "reference";
    case 16: return "value";
    case 18: return "enum";
    case 19: return "flags";
    case 20: return "name";
    case 23: return "string";
    default: return nullptr;
    }
}

void appendf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

double entropyOf(const uint8_t* d, size_t len) {
    if (!len) return 0;
    size_t hist[256] = {0};
    for (size_t i = 0; i < len; ++i) hist[d[i]]++;
    double e = 0;
    for (int i = 0; i < 256; ++i) {
        if (!hist[i]) continue;
        double p = (double)hist[i] / (double)len;
        e -= p * std::log2(p);
    }
    return e;
}

std::string hexDump(const uint8_t* d, size_t len, size_t limit) {
    std::string s;
    size_t n = std::min(len, limit);
    for (size_t off = 0; off < n; off += 16) {
        appendf(s, "%08X  ", (unsigned)off);
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < n) appendf(s, "%02X ", d[off + i]);
            else s += "   ";
            if (i == 7) s += " ";
        }
        s += " |";
        for (size_t i = 0; i < 16 && off + i < n; ++i) {
            uint8_t c = d[off + i];
            s += (c >= 32 && c < 127) ? (char)c : '.';
        }
        s += "|\r\n";
    }
    if (n < len) appendf(s, "... %u more bytes\r\n", (unsigned)(len - n));
    return s;
}

} // namespace

std::string sbinText(const uint8_t* d, size_t len) {
    std::string s;
    auto chunks = sbinChunks(d, len);
    if (chunks.empty()) return s;

    uint32_t version = 0;
    if (len >= 8) memcpy(&version, d + 4, 4);
    appendf(s, "SBIN version %u, %u bytes\r\n\r\n", version, (unsigned)len);

    auto names = sbinNames(chunks);
    const SbinChunk* stru = findChunk(chunks, "STRU");
    const SbinChunk* fiel = findChunk(chunks, "FIEL");
    const SbinChunk* enm  = findChunk(chunks, "ENUM");

    s += "chunks\r\n";
    for (const auto& c : chunks) {
        appendf(s, "  %-4s %9u bytes", c.tag, (unsigned)c.size);
        if (!memcmp(c.tag, "STRU", 4)) appendf(s, "   %u structure(s)", (unsigned)(c.size / 6));
        else if (!memcmp(c.tag, "FIEL", 4)) appendf(s, "   %u field(s)", (unsigned)(c.size / 8));
        else if (!memcmp(c.tag, "ENUM", 4)) appendf(s, "   %u enum(s)", (unsigned)(c.size / 8));
        else if (!memcmp(c.tag, "CHDR", 4)) appendf(s, "   %u name(s)", (unsigned)(c.size / 8));
        s += "\r\n";
    }

    auto nameAt = [&](size_t i) -> std::string {
        return i < names.size() ? names[i] : std::string("?");
    };

    // Which enum a field uses is the last word of its FIEL entry: an index
    // into ENUM. geoType carries 1 and ENUM[1] is ImplicitGeoShape, deathEvent
    // carries 5 and ENUM[5] is EventID - both exactly right.
    std::vector<std::string> enumNames;
    if (enm) {
        for (size_t i = 0; i < enm->size / 8; ++i) {
            uint16_t nameIdx;
            memcpy(&nameIdx, enm->data + i * 8, 2);
            enumNames.push_back(nameAt(nameIdx));
        }
    }

    // Names that are declared as something else mark where an enum's member
    // list ends: the file gives no member count, but a member is never also a
    // field or a structure name.
    // Marked by text, not by index: the name table stores the same word more
    // than once, so marking one index would leave the copies unmarked and an
    // enum would swallow the next structure's fields.
    std::vector<uint8_t> declared(names.size(), 0);
    auto markDeclared = [&](size_t i) {
        if (i >= names.size()) return;
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] == names[i]) declared[k] = 1;
    };
    if (fiel) {
        for (size_t f = 0; f < fiel->size / 8; ++f) {
            uint16_t fn;
            memcpy(&fn, fiel->data + f * 8, 2);
            markDeclared(fn);
        }
    }
    if (stru) {
        for (size_t i = 0; i < stru->size / 6; ++i) {
            uint16_t nameIdx;
            memcpy(&nameIdx, stru->data + i * 6, 2);
            markDeclared(nameIdx);
        }
    }
    if (enm) {
        for (size_t i = 0; i < enm->size / 8; ++i) {
            uint16_t nameIdx;
            memcpy(&nameIdx, enm->data + i * 8, 2);
            markDeclared(nameIdx);
        }
    }

    if (stru && fiel) {
        size_t nStru = stru->size / 6, nFiel = fiel->size / 8;
        appendf(s, "\r\nstructures (%u)\r\n", (unsigned)nStru);
        for (size_t i = 0; i < nStru; ++i) {
            uint16_t nameIdx, first, count;
            memcpy(&nameIdx, stru->data + i * 6, 2);
            memcpy(&first,   stru->data + i * 6 + 2, 2);
            memcpy(&count,   stru->data + i * 6 + 4, 2);
            appendf(s, "  %s\r\n", nameAt(nameIdx).c_str());
            for (size_t f = first; f < (size_t)first + count && f < nFiel; ++f) {
                uint16_t fn, ft, off, flags;
                memcpy(&fn,    fiel->data + f * 8, 2);
                memcpy(&ft,    fiel->data + f * 8 + 2, 2);
                memcpy(&off,   fiel->data + f * 8 + 4, 2);
                memcpy(&flags, fiel->data + f * 8 + 6, 2);
                const char* tn = version == 3 ? sbinVersion3TypeName(ft) : fieldTypeName(ft);
                char typeBuf[24];
                if (!tn) { snprintf(typeBuf, sizeof(typeBuf), "type%u", ft); tn = typeBuf; }
                appendf(s, "      %-10s %-28s at %u", tn, nameAt(fn).c_str(), off);
                if ((ft == 18 || ft == 19) && flags < enumNames.size())
                    appendf(s, "  %s", enumNames[flags].c_str());
                else if (flags)
                    appendf(s, "  (%u)", flags);
                s += "\r\n";
            }
        }
    }

    if (enm) {
        size_t nEnum = enm->size / 8;
        appendf(s, "\r\nenumerations (%u)\r\n", (unsigned)nEnum);
        std::vector<std::string> done;
        for (size_t i = 0; i < nEnum; ++i) {
            uint16_t nameIdx;
            memcpy(&nameIdx, enm->data + i * 8, 2);
            std::string name = nameAt(nameIdx);
            // the same enum is listed once per field that uses it
            if (std::find(done.begin(), done.end(), name) != done.end()) continue;
            done.push_back(name);
            appendf(s, "  %s:", name.c_str());
            // members follow the enum's own name and run until the next name
            // the file declares as a field, structure or enum
            bool any = false;
            for (size_t k = (size_t)nameIdx + 1; k < names.size(); ++k) {
                if (declared[k] || names[k].empty()) break;
                appendf(s, "%s %s", any ? "," : "", names[k].c_str());
                any = true;
            }
            if (!any) s += " (members not listed in this file)";
            s += "\r\n";
        }
    }

    s += sbinObjectsText(d, len);

    appendf(s, "\r\nnames (%u)\r\n", (unsigned)names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i].empty()) continue;
        appendf(s, "  [%u] %s\r\n", (unsigned)i, names[i].c_str());
    }
    return s;
}

std::string assetText(const std::string& assetPath, const uint8_t* d, size_t len) {
    std::string s;
    std::string ext = extensionOf(assetPath);
    appendf(s, "%s\r\n%u bytes\r\n\r\n", assetPath.c_str(), (unsigned)len);

    if (len >= 4 && !memcmp(d, "SBIN", 4)) {
        s += sbinText(d, len);
        return s;
    }
    // No Limits' encrypted data files: decrypted with the game's key and the
    // file's own name, then shown like any other SBIN
    if (ext == "sb" && len >= 16 && len % 16 == 0) {
        Bytes sbin;
        std::string how;
        if (nlSbDecode(assetPath, d, len, sbin, &how)) {
            appendf(s, "Decrypted (%s) - %u bytes of SBIN inside.\r\n"
                       "File > Save as... writes the decrypted SBIN (.sbin) or this text.\r\n\r\n",
                    how.c_str(), (unsigned)sbin.size());
            s += sbinText(sbin.data(), sbin.size());
            return s;
        }
    }
    if (ext == "nct" || ext == "gui") return nctText(assetPath, d, len);
    if (len >= 32 && memcmp(d, "Gnsu", 4) == 0) {
        GnsuInfo g;
        readGnsuInfo(d, len, g);
        appendf(s, "EA granular engine bank (Gnsu%s)\r\n"
                   "  rev range     %.0f - %.0f rpm\r\n"
                   "  rev table     %u entries\r\n"
                   "  grains        %u\r\n"
                   "  samples       %u at %u Hz (%.1f s), mono, EA-XAS\r\n\r\n"
                   "The game crossfades between grains as the engine speed changes.\r\n"
                   "Save as .wav to hear every grain in order, idle to redline.\r\n",
                g.version.c_str(), g.minRpm, g.maxRpm, g.entryCount, g.grainCount,
                g.totalSamples, g.sampleRate,
                g.sampleRate ? (double)g.totalSamples / g.sampleRate : 0.0);
        return s;
    }
    if (isJsr184(d, len)) {
        std::string map = m3gObjectMap(d, len);
        if (!map.empty()) return s + map;
    }
    if (ext == "points") {
        std::vector<Hardpoint> pts = rr3ReadPoints(d, len);
        if (!pts.empty()) {
            s += "Hardpoints, in the model's own units and axes\r\n"
                 "(the file states them at 32 units to one, in the authoring\r\n"
                 "program's axis order; both are converted here)\r\n\r\n";
            for (const Hardpoint& h : pts) {
                appendf(s, "  %-26s %9.3f %9.3f %9.3f%s\r\n", h.name.c_str(),
                        h.pos[0], h.pos[1], h.pos[2], h.hinge ? "   hinge" : "");
                if (h.hinge)
                    appendf(s, "  %-26s axes   %6.3f %6.3f %6.3f | %6.3f %6.3f %6.3f | "
                               "%6.3f %6.3f %6.3f\r\n", "",
                            h.basis[0], h.basis[1], h.basis[2],
                            h.basis[3], h.basis[4], h.basis[5],
                            h.basis[6], h.basis[7], h.basis[8]);
            }
            s += "\r\nOpen the model beside this file and these are applied to it:\r\n"
                 "parts modelled at the origin are moved onto the point they\r\n"
                 "belong to, and every point is exported as an empty.\r\n";
            return s;
        }
    }
    if (len >= 128 && !memcmp(d, "DDS ", 4)) {
        auto u32 = [&](size_t off) {
            return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
                   ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
        };
        char fourcc[5] = {0, 0, 0, 0, 0};
        memcpy(fourcc, d + 84, 4);
        bool named = fourcc[0] >= 32 && fourcc[0] < 127;
        appendf(s, "DDS texture\r\n  size        %u x %u\r\n  mip levels  %u\r\n"
                   "  payload     %s\r\n",
                u32(16), u32(12), u32(28),
                named ? fourcc : "uncompressed (RGBA4444)");
        Image img;
        if (decodeDdsContainer(d, len, img))
            appendf(s, "  decoded     %d x %d, %d channels\r\n", img.width,
                    img.height, img.channels);
        else
            s += "  decoded     no - this codec has no decoder here\r\n";
        s += "\r\n";
        return s;
    }
    if (len >= 4 && !memcmp(d, "BKHD", 4)) {
        Bank bank;
        if (readBank(d, len, bank)) {
            s += describeBank(bank);
            s += "\r\n";
            return s;
        }
    }
    if (len >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
        WemInfo w;
        if (readWemInfo(d, len, w)) {
            appendf(s, "Wwise sound\r\n  codec       0x%04X (%s)\r\n  channels    %u\r\n"
                       "  sample rate %u Hz\r\n  samples     %u\r\n  audio data  %u bytes\r\n",
                    w.codec, wemCodecName(w.codec), w.channels, w.sampleRate,
                    w.totalSamples, (unsigned)w.dataSize);
            if (w.codec == 0xFFFF) {
                appendf(s, "  block sizes 2^%u / 2^%u\r\n  setup packet at %u, first audio packet at %u\r\n"
                           "  codebooks   %s\r\n",
                        w.blockSize0, w.blockSize1, w.setupPacketOffset,
                        w.firstAudioPacketOffset,
                        w.inlineSetup ? "inside the file" : "external (Wwise codebook library)");
            }
            return s;
        }
    }

    if (len >= 10 && d[0] == 0xDA && d[1] == 0xBD) {
        size_t inside = 0;
        m3gWrapperInfo(d, len, &inside);
        uint32_t comp = 0;
        memcpy(&comp, d + 6, 4);
        appendf(s, "Compressed model (the game's own DA BD wrapper)\r\n"
                   "  contents   %u bytes when unpacked\r\n"
                   "  payload    %u bytes\r\n"
                   "  ratio      %.2fx\r\n"
                   "  codec      LZHAM (%s)\r\n\r\n",
                (unsigned)inside, comp, comp ? (double)inside / comp : 0.0,
                lzhamAvailable() ? lzhamBackend().c_str()
                                 : "no decoder loaded - see below");
        if (len >= 22) {
            const uint8_t* p = d + 10;
            appendf(s, "  header     %02X %02X %02X %02X | %02X %02X %02X %02X | "
                       "%02X %02X %02X %02X\r\n"
                       "             byte 0 is the dictionary size (0x16 = 22, 4 MB),\r\n"
                       "             bytes 4-7 the Adler-32 - it comes back reversed as\r\n"
                       "             the last four bytes of the file - and the stream\r\n"
                       "             itself starts at byte 8.\r\n\r\n",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                    p[8], p[9], p[10], p[11]);
        }
        {
            std::string map = m3gObjectMap(d, len);
            if (!map.empty()) return s + "Unpacked:\r\n\r\n" + map;
        }
        if (!lzhamAvailable())
            s += "The same compressor packs the game's own archives, so it is not a\r\n"
                 "guess. No LZHAM decoder is loaded: put lzham_x64.dll beside the\r\n"
                 "program - BUILD.bat builds one - and this model converts like any\r\n"
                 "other. Save it as raw to get the payload on its own.\r\n\r\n";
    }

    // Not a format with a schema. Say what can be said about the bytes, then
    // print them.
    double e = entropyOf(d, len);
    appendf(s, "no readable structure: entropy %.2f bits/byte", e);
    if (len && len % 16 == 0 && e > 7.5) {
        s += ", size is an exact multiple of 16\r\n"
             "This file is encrypted. Every .sb in the game is a whole number of\r\n"
             "16-byte blocks with no repeated blocks inside a file, which is a block\r\n"
             "cipher in CBC mode; two cars' files share only their first block. The\r\n"
             "key is held by the game binary, so the contents cannot be shown here.\r\n";
    } else {
        s += "\r\n";
    }
    s += "\r\n";
    s += hexDump(d, len, 64 * 1024);
    (void)ext;
    return s;
}

} // namespace nfsnl
