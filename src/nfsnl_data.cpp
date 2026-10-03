// Data files as editable text, and back.
//
// Real Racing 3 keeps much of its game data in small binary files with no
// published layout: .evt (a track's grid and camera poses), .sounddef (which
// .wav files a sound plays, how loud, how varied) and plain .dat tables. Each
// becomes text here that an editor can change and that encodes back to the
// exact bytes when nothing was changed:
//
//   .sounddef   its fields by name: name, group, mode, values, one line per sample
//   anything else a reversible listing - str "..." for each zero-terminated
//               string, f32 / u32 / i32 for four-byte values, hex for the rest
//
// Files that are encrypted (the .nct tables, .cc_cust, prof.dat - their bytes
// are indistinguishable from noise) are refused rather than shown as rubbish.

#include "nfsnl.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace nfsnl {

namespace {

uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
void put32(Bytes& b, uint32_t v) { for (int k = 0; k < 4; ++k) b.push_back((uint8_t)(v >> (8 * k))); }

bool printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

std::string quote(const uint8_t* s, size_t n) {
    std::string o = "\"";
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else if (printable(c)) o += (char)c;
        else { char b[8]; snprintf(b, sizeof(b), "\\x%02X", c); o += b; }
    }
    return o + "\"";
}

// a quoted string at `p` in `line`; false when malformed
bool unquote(const std::string& line, size_t& p, std::string& out) {
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p >= line.size() || line[p] != '"') return false;
    ++p;
    out.clear();
    while (p < line.size() && line[p] != '"') {
        char c = line[p++];
        if (c != '\\') { out += c; continue; }
        if (p >= line.size()) return false;
        char e = line[p++];
        if (e == 'n') out += '\n';
        else if (e == 't') out += '\t';
        else if (e == 'r') out += '\r';
        else if (e == 'x' && p + 2 <= line.size()) {
            out += (char)strtoul(line.substr(p, 2).c_str(), nullptr, 16);
            p += 2;
        } else out += e;
    }
    if (p >= line.size()) return false;
    ++p;
    return true;
}

// the float's shortest exact spelling, or "" when text cannot carry it exactly
std::string exactFloat(uint32_t bits) {
    float f; memcpy(&f, &bits, 4);
    if (!std::isfinite(f)) return "";
    char b[48];
    for (int prec = 6; prec <= 9; ++prec) {
        snprintf(b, sizeof(b), "%.*g", prec, f);
        float g = strtof(b, nullptr);
        uint32_t gb; memcpy(&gb, &g, 4);
        if (gb == bits) return b;
    }
    return "";
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        if (e >= text.size()) break;
        p = e + 1;
    }
    return out;
}

std::string firstWord(const std::string& l, size_t& p) {
    p = 0;
    while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) ++p;
    size_t b = p;
    while (p < l.size() && l[p] != ' ' && l[p] != '\t') ++p;
    return l.substr(b, p - b);
}

// ---------------------------------------------------------------- .sounddef

struct SoundDef {
    uint32_t version = 3, mode = 0;
    std::string name, group;
    std::vector<uint32_t> values;           // raw bits, shown as floats
    struct Sample { std::string path; uint32_t weight = 100, flag = 0; };
    std::vector<Sample> samples;
    Bytes tail;
};

bool readSoundDef(const uint8_t* d, size_t n, SoundDef& s) {
    size_t p = 0;
    auto u32 = [&](uint32_t& v) { if (p + 4 > n) return false; v = le32(d + p); p += 4; return true; };
    auto str = [&](std::string& v) {
        uint32_t l;
        if (!u32(l) || l > 4096 || p + l > n) return false;
        v.assign((const char*)d + p, l);
        p += l;
        return true;
    };
    if (!u32(s.version) || s.version < 1 || s.version > 16) return false;
    if (!str(s.name) || !str(s.group) || !u32(s.mode)) return false;
    // values until the sample count: a count followed by a path that is a
    // printable string
    for (int k = 0; k < 32; ++k) {
        if (p + 8 <= n) {
            uint32_t c = le32(d + p), l = le32(d + p + 4);
            if (c >= 1 && c <= 256 && l >= 3 && l <= 1024 && p + 8 + l <= n) {
                bool ok = true;
                for (uint32_t i = 0; i < l && ok; ++i) ok = printable(d[p + 8 + i]);
                if (ok) break;
            }
            if (c == 0 && p + 4 == n) break;
        }
        uint32_t v;
        if (!u32(v)) return false;
        s.values.push_back(v);
    }
    uint32_t count;
    if (!u32(count) || count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        SoundDef::Sample sm;
        if (!str(sm.path) || !u32(sm.weight) || !u32(sm.flag)) return false;
        s.samples.push_back(sm);
    }
    s.tail.assign(d + p, d + n);
    return true;
}

bool isSoundDefName(const std::string& path) { return extensionOf(path) == "sounddef"; }

// an encrypted or compressed file: bytes spread evenly over all 256 values
bool looksEncrypted(const uint8_t* d, size_t n) {
    if (n < 96) {
        // too short to measure: no zero bytes and no text at all
        size_t zeros = 0, text = 0;
        for (size_t i = 0; i < n; ++i) { if (!d[i]) ++zeros; if (printable(d[i])) ++text; }
        return n >= 32 && zeros == 0 && text < n / 2;
    }
    size_t count[256] = {0};
    size_t m = std::min<size_t>(n, 1 << 16);
    for (size_t i = 0; i < m; ++i) ++count[d[i]];
    double h = 0;
    for (size_t c : count) if (c) { double q = (double)c / m; h -= q * std::log2(q); }
    double limit = m >= 4096 ? 7.6 : (m >= 512 ? 7.0 : 6.2);
    return h > limit;
}

bool looksText(const uint8_t* d, size_t n) {
    if (!n) return true;
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = d[i];
        if (c == 0) return false;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ++bad;
    }
    return bad * 100 < n;
}

bool textExtension(const std::string& e) {
    return e == "txt" || e == "xml" || e == "json" || e == "ini" || e == "cfg" || e == "config" ||
           e == "lua" || e == "csv" || e == "utf8" || e == "info" || e == "html" || e == "plist";
}

} // namespace

bool dataIsEncrypted(const uint8_t* d, size_t n) { return looksEncrypted(d, n); }

std::string dataToText(const uint8_t* d, size_t n) {
    std::string o;
    std::string hexRun;
    int hexCount = 0;
    auto flushHex = [&]() {
        if (hexCount) { o += "hex" + hexRun + "\n"; hexRun.clear(); hexCount = 0; }
    };
    // a string's length with its zero, or 0; right after another string one
    // printable character is enough ("gridPosition", "1")
    bool afterString = false;
    auto stringAt = [&](size_t p, size_t minLen) -> size_t {
        size_t e = p;
        while (e < n && printable(d[e])) ++e;
        if (e < n && d[e] == 0 && e - p >= minLen) return e - p + 1;
        return 0;
    };
    size_t p = 0;
    while (p < n) {
        size_t sl = stringAt(p, afterString ? 1 : 3);
        if (sl) {
            flushHex();
            o += "str " + quote(d + p, sl - 1) + "\n";
            p += sl;
            afterString = true;
            continue;
        }
        afterString = false;
        // a two-byte count before a string
        if (p + 2 < n && stringAt(p + 2, 3) && !stringAt(p + 1, 3)) {
            flushHex();
            o += "u16 " + std::to_string(d[p] | (d[p + 1] << 8)) + "\n";
            p += 2;
            continue;
        }
        // a string starting inside the next four bytes: the bytes before it go raw
        size_t k = 1;
        for (; k < 4 && p + k < n; ++k) if (stringAt(p + k, 3)) break;
        if (p + 4 > n || (k < 4 && p + k < n)) {
            size_t take = (p + 4 > n) ? n - p : k;
            for (size_t i = 0; i < take; ++i) {
                char b[8]; snprintf(b, sizeof(b), " %02X", d[p + i]);
                hexRun += b;
                if (++hexCount == 16) flushHex();
            }
            p += take;
            continue;
        }
        uint32_t v = le32(d + p);
        int32_t sv = (int32_t)v;
        std::string f = exactFloat(v);
        float fv; memcpy(&fv, &v, 4);
        std::string line;
        if (v < (1u << 20)) line = "u32 " + std::to_string(v);
        else if (sv < 0 && sv > -(1 << 20)) line = "i32 " + std::to_string(sv);
        else if (!f.empty() && std::fabs(fv) >= 1e-6f && std::fabs(fv) <= 1e9f) line = "f32 " + f;
        if (line.empty()) {
            for (int i = 0; i < 4; ++i) {
                char b[8]; snprintf(b, sizeof(b), " %02X", d[p + i]);
                hexRun += b;
                if (++hexCount == 16) flushHex();
            }
        } else {
            flushHex();
            o += line + "\n";
        }
        p += 4;
    }
    flushHex();
    return o;
}

bool dataFromText(const std::string& text, Bytes& out, std::string& why) {
    out.clear();
    int ln = 0;
    for (const std::string& l : lines(text)) {
        ++ln;
        size_t p;
        std::string w = firstWord(l, p);
        if (w.empty() || w[0] == '#') continue;
        auto fail = [&](const char* what) {
            why = "line " + std::to_string(ln) + ": " + what + "\n    " + l;
            return false;
        };
        if (w == "str") {
            std::string s;
            if (!unquote(l, p, s)) return fail("a str needs its text in quotes");
            out.insert(out.end(), s.begin(), s.end());
            out.push_back(0);
        } else if (w == "u32" || w == "i32" || w == "f32" || w == "u16" || w == "u8") {
            std::string rest = l.substr(p);
            char* end = nullptr;
            const char* c = rest.c_str();
            while (*c == ' ' || *c == '\t') ++c;
            if (w == "f32") {
                float f = strtof(c, &end);
                if (end == c) return fail("f32 needs a number");
                uint32_t b; memcpy(&b, &f, 4);
                put32(out, b);
            } else {
                long long v = strtoll(c, &end, 0);
                if (end == c) return fail("needs a whole number");
                if (w == "u8") out.push_back((uint8_t)v);
                else if (w == "u16") { out.push_back((uint8_t)v); out.push_back((uint8_t)(v >> 8)); }
                else put32(out, (uint32_t)v);
            }
        } else if (w == "hex") {
            std::string rest = l.substr(p);
            size_t q = 0;
            while (q < rest.size()) {
                while (q < rest.size() && (rest[q] == ' ' || rest[q] == '\t')) ++q;
                if (q >= rest.size()) break;
                if (q + 2 > rest.size() || !isxdigit((unsigned char)rest[q]) || !isxdigit((unsigned char)rest[q + 1]))
                    return fail("hex takes pairs of hex digits");
                out.push_back((uint8_t)strtoul(rest.substr(q, 2).c_str(), nullptr, 16));
                q += 2;
            }
        } else {
            return fail("unknown line (str, u32, i32, f32, u16, u8 or hex)");
        }
    }
    return true;
}

bool dataEditableText(const std::string& path, const uint8_t* d, size_t n,
                      std::string& text, std::string& why) {
    std::string e = extensionOf(path);
    if (isSoundDefName(path)) {
        SoundDef s;
        if (readSoundDef(d, n, s)) {
            text = "# " + baseName(path) + " - a Real Racing 3 sound definition\n"
                   "# name and group are the sound's own; values are its settings as the file keeps\n"
                   "# them (volume and pitch variation, gain in dB...); each sample line is a .wav\n"
                   "# the sound plays with the two numbers the file keeps beside it (weight, flag).\n"
                   "# Add or remove sample lines to change what it plays.\n";
            text += "version " + std::to_string(s.version) + "\n";
            text += "name " + quote((const uint8_t*)s.name.data(), s.name.size()) + "\n";
            text += "group " + quote((const uint8_t*)s.group.data(), s.group.size()) + "\n";
            text += "mode " + std::to_string(s.mode) + "\n";
            text += "values";
            for (uint32_t v : s.values) {
                std::string f = exactFloat(v);
                char hx[16]; snprintf(hx, sizeof(hx), "0x%08X", v);
                text += " " + (f.empty() ? std::string(hx) : f);
            }
            text += "\n";
            for (const SoundDef::Sample& sm : s.samples)
                text += "sample " + quote((const uint8_t*)sm.path.data(), sm.path.size()) + " " +
                        std::to_string(sm.weight) + " " + std::to_string(sm.flag) + "\n";
            if (!s.tail.empty()) {
                text += "tail";
                for (uint8_t b : s.tail) { char h[8]; snprintf(h, sizeof(h), " %02X", b); text += h; }
                text += "\n";
            }
            return true;
        }
    }
    if (textExtension(e) || looksText(d, n)) {
        if (!looksText(d, n)) { why = "this file is not plain text"; return false; }
        text.assign((const char*)d, n);
        return true;
    }
    if (looksEncrypted(d, n)) {
        why = "this file is encrypted (its bytes look like noise), so there is nothing here "
              "to edit: the game decrypts it with a key the tool does not have.";
        return false;
    }
    text = "# " + baseName(path) + " as editable text: str \"...\" is a zero-terminated string,\n"
           "# f32 a 4-byte float, u32 / i32 a 4-byte whole number, hex raw bytes.\n"
           "# Change values in place; changing a string's length is fine where the game\n"
           "# reads strings to their end (.evt, .dat tables), not where it keeps offsets.\n";
    text += dataToText(d, n);
    return true;
}

bool dataFromEditedText(const std::string& path, const std::string& text,
                        const uint8_t* original, size_t originalLen,
                        Bytes& out, std::string& why) {
    std::string e = extensionOf(path);
    std::vector<std::string> ls = lines(text);
    bool soundDef = false;
    for (const std::string& l : ls) {
        size_t p;
        std::string w = firstWord(l, p);
        if (w.empty() || w[0] == '#') continue;
        soundDef = w == "version" && isSoundDefName(path);
        break;
    }
    if (soundDef) {
        SoundDef s;
        bool haveName = false;
        int ln = 0;
        for (const std::string& l : ls) {
            ++ln;
            size_t p;
            std::string w = firstWord(l, p);
            if (w.empty() || w[0] == '#') continue;
            auto fail = [&](const char* what) { why = "line " + std::to_string(ln) + ": " + what + "\n    " + l; return false; };
            if (w == "version") s.version = (uint32_t)strtoul(l.c_str() + p, nullptr, 0);
            else if (w == "mode") s.mode = (uint32_t)strtoul(l.c_str() + p, nullptr, 0);
            else if (w == "name") { if (!unquote(l, p, s.name)) return fail("name needs quotes"); haveName = true; }
            else if (w == "group") { if (!unquote(l, p, s.group)) return fail("group needs quotes"); }
            else if (w == "values") {
                const char* c = l.c_str() + p;
                for (;;) {
                    while (*c == ' ' || *c == '\t') ++c;
                    if (!*c) break;
                    char* end = nullptr;
                    uint32_t bits;
                    if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) bits = (uint32_t)strtoul(c + 2, &end, 16);
                    else { float f = strtof(c, &end); memcpy(&bits, &f, 4); }
                    if (end == c) return fail("values takes numbers");
                    s.values.push_back(bits);
                    c = end;
                }
            } else if (w == "sample") {
                SoundDef::Sample sm;
                if (!unquote(l, p, sm.path)) return fail("sample needs its .wav path in quotes");
                const char* c = l.c_str() + p;
                char* end = nullptr;
                sm.weight = (uint32_t)strtoul(c, &end, 0);
                if (end != c) { c = end; sm.flag = (uint32_t)strtoul(c, &end, 0); }
                else sm.weight = 100;
                s.samples.push_back(sm);
            } else if (w == "tail") {
                const char* c = l.c_str() + p;
                for (;;) {
                    while (*c == ' ') ++c;
                    if (!*c) break;
                    char* end = nullptr;
                    unsigned long b = strtoul(c, &end, 16);
                    if (end == c) return fail("tail takes hex bytes");
                    s.tail.push_back((uint8_t)b);
                    c = end;
                }
            } else return fail("unknown line (version, name, group, mode, values, sample, tail)");
        }
        if (!haveName) { why = "the sound needs a name line"; return false; }
        if (s.samples.empty()) { why = "the sound needs at least one sample line"; return false; }
        out.clear();
        put32(out, s.version);
        put32(out, (uint32_t)s.name.size()); out.insert(out.end(), s.name.begin(), s.name.end());
        put32(out, (uint32_t)s.group.size()); out.insert(out.end(), s.group.begin(), s.group.end());
        put32(out, s.mode);
        for (uint32_t v : s.values) put32(out, v);
        put32(out, (uint32_t)s.samples.size());
        for (const SoundDef::Sample& sm : s.samples) {
            put32(out, (uint32_t)sm.path.size());
            out.insert(out.end(), sm.path.begin(), sm.path.end());
            put32(out, sm.weight);
            put32(out, sm.flag);
        }
        out.insert(out.end(), s.tail.begin(), s.tail.end());
        return true;
    }
    // plain text stays text
    bool wasText = original ? (textExtension(e) || looksText(original, originalLen)) : textExtension(e);
    if (wasText) {
        out.assign(text.begin(), text.end());
        return true;
    }
    return dataFromText(text, out, why);
}

std::vector<std::string> soundDefSamples(const uint8_t* d, size_t n) {
    std::vector<std::string> out;
    SoundDef s;
    if (readSoundDef(d, n, s))
        for (const SoundDef::Sample& sm : s.samples) out.push_back(sm.path);
    return out;
}

} // namespace nfsnl
