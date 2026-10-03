// nfsnl_library.cpp - the asset library: index now, decompress later
//
// Version 0.4 unpacked the whole game into a temp folder before showing
// anything. On a full install that is several gigabytes written to
// AppData\Local\Temp, which the program then had to delete again, and which a
// crash or a forced close left behind.
//
// This reads only the manifest of each archive - a few kilobytes each - and
// remembers where every asset lives: which archive, which cabinet, and the
// offset inside it. An asset's bytes are produced the moment they are wanted,
// by reading that one cabinet's slice off disk and decompressing it in
// memory. Nothing is written anywhere.
//
// A cabinet holds many assets, so the last few are kept in a small cache: a
// car's model and all of its textures usually sit in the same cabinet, which
// means one decompression serves the lot.
#include "nfsnl.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace nfsnl {

namespace {

// Read `length` bytes at `offset` without loading the whole file.
bool readRange(const std::string& path, size_t offset, size_t length, Bytes& out) {
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    // an Android .obb can pass 2 GB, so the seek has to be 64-bit
#ifdef _WIN32
    if (_fseeki64(f, (long long)offset, SEEK_SET) != 0) { fclose(f); return false; }
#else
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) { fclose(f); return false; }
#endif
    out.resize(length);
    size_t got = length ? fread(out.data(), 1, length, f) : 0;
    fclose(f);
    if (got != length) { out.clear(); return false; }
    return true;
}

} // namespace

bool indexArchive(const std::string& path, Library& lib) {
    std::string label = stripExtension(baseName(path));

    // The header and the gzipped manifest sit at the front of the file, and
    // the manifest is small, so a modest slice is enough to index an archive
    // of any size. Only if the manifest turns out to be larger is the rest
    // read as well.
    Bytes head;
    size_t want = 1u << 20;
    if (!readRange(path, 0, want, head)) {
        if (!readFile(path, head)) {
            lib.warnings.push_back(label + ": cannot read file");
            return false;
        }
    }
    if (head.size() < 20 || memcmp(head.data(), "PACK", 4) != 0) {
        if (head.size() >= 4 && memcmp(head.data(), "SBIN", 4) == 0) {
            LibraryEntry e;
            e.path = "_unnamed/" + label + ".sbin";
            e.kind = "other";
            // An SBIN with geometry in it (BULK/BARG chunks, as the cars'
            // .sb3d have) is a model, whatever its missing name - worth
            // showing as one, since these standalone files are where the
            // game keeps content its packs do not name.
            for (size_t k = 8; k + 12 <= head.size(); ) {
                if (!memcmp(head.data() + k, "BULK", 4) || !memcmp(head.data() + k, "BARG", 4)) {
                    e.path = "_unnamed/" + label + ".sb3d";
                    e.kind = "model";
                    break;
                }
                uint32_t sz;
                memcpy(&sz, head.data() + k + 4, 4);
                if (sz > head.size()) break;
                k += 12 + (((size_t)sz + 3) & ~(size_t)3);
            }
            e.archive = (int)lib.archives.size();
            e.whole = true;
            e.length = (uint32_t)fileSizeOf(path);   // the whole file is the asset
            lib.entries.push_back(e);
            Archive a;
            a.path = path;
            lib.archives.push_back(std::move(a));
            return true;
        }
        lib.warnings.push_back(label + ": unrecognised container");
        return false;
    }
    if (memcmp(head.data() + 12, "ZBDS", 4) != 0) {
        lib.warnings.push_back(label + ": PACK header missing ZBDS");
        return false;
    }

    Bytes meta;
    size_t consumed = 0;
    if (!inflateGzip(head.data() + 20, head.size() - 20, meta, &consumed)) {
        // manifest longer than the slice that was read - take the whole file
        Bytes all;
        if (!readFile(path, all) ||
            !inflateGzip(all.data() + 20, all.size() - 20, meta, &consumed)) {
            lib.warnings.push_back(label + ": manifest gzip decode failed");
            return false;
        }
        head.swap(all);
    }
    size_t bulkStart = 20 + consumed;
    size_t dataBase = (bulkStart + 15) & ~(size_t)15;
    if (meta.size() < 4 || memcmp(meta.data(), "SBIN", 4) != 0) {
        lib.warnings.push_back(label + ": manifest is not SBIN");
        return false;
    }

    size_t fileSize = fileSizeOf(path);
    PackManifest man = parsePackManifest(meta, fileSize, dataBase);
    if (!man.valid) {
        lib.warnings.push_back(label + ": no usable manifest records");
        return false;
    }
    auto paths = man.resolvePaths();
    if (paths.empty()) {
        lib.warnings.push_back(label + ": could not resolve file paths");
        return false;
    }

    Archive arch;
    arch.path = path;
    arch.dataBase = dataBase;
    arch.size = fileSize;
    arch.cabinets = man.cabinets;
    int archiveIndex = (int)lib.archives.size();

    // Which cabinets can actually be read. An external one whose .cab was
    // never downloaded, or an internal one that runs past the end of a
    // truncated .pack, would only put names in the tree that fail when
    // clicked - so their assets are left out of the list altogether.
    std::vector<char> present(man.cabinets.size(), 1);
    {
        size_t slash = path.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
        for (size_t c = 0; c < man.cabinets.size(); ++c) {
            const Cabinet& cab = man.cabinets[c];
            if (cab.flags & CAB_EXTERNAL) {
                char leaf[32];
                snprintf(leaf, sizeof(leaf), "%u.cab", (unsigned)c);
#ifdef _WIN32
                std::string cabPath = dir + label + "\\" + leaf;
#else
                std::string cabPath = dir + label + "/" + leaf;
#endif
                present[c] = fileSizeOf(cabPath) > 0;
            } else if (fileSize &&
                       (uint64_t)cab.packedOffset + cab.packedLength + dataBase > fileSize) {
                present[c] = 0;
            }
        }
    }
    size_t hidden = 0;

    std::string prefix = man.variant.empty() ? "" : man.variant + "/";
    for (auto& kv : paths) {
        const PackFile& pf = man.files[kv.first];
        if (pf.cabinetIndex >= present.size() || !present[pf.cabinetIndex]) {
            ++hidden;
            continue;
        }
        LibraryEntry e;
        e.path = prefix + kv.second;
        e.kind = classifyByName(e.path);
        e.archive = archiveIndex;
        e.cabinet = pf.cabinetIndex;
        e.offset = pf.offset;
        e.length = pf.length;
        lib.entries.push_back(std::move(e));
    }
    lib.archives.push_back(std::move(arch));
    if (hidden) {
        char msg[160];
        snprintf(msg, sizeof(msg), ": %u asset(s) not listed - their cabinet is not on disk"
                 " (not downloaded yet?)", (unsigned)hidden);
        lib.warnings.push_back(label + msg);
    }
    return true;
}

static bool indexFmobbAt(const std::string& diskPath, uint64_t base, uint64_t size,
                         const std::string& relPrefix, Library& lib);

bool indexZipArchive(const std::string& diskPath, const std::string& relPrefix, Library& lib) {
    size_t size = fileSizeOf(diskPath);
    if (size < 22) return false;
    // the end-of-central-directory record is in the last 64 KB + 22 bytes
    size_t tail = std::min<size_t>(size, 65557);
    Bytes t;
    if (!readRange(diskPath, size - tail, tail, t)) return false;
    size_t eocd = std::string::npos;
    for (size_t i = t.size() - 22 + 1; i-- > 0;)
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6) { eocd = i; break; }
    if (eocd == std::string::npos) return false;
    auto r16 = [](const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8)); };
    auto r32 = [](const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };
    uint32_t count = r16(t.data() + eocd + 10);
    uint32_t cdSize = r32(t.data() + eocd + 12), cdOff = r32(t.data() + eocd + 16);
    if ((size_t)cdOff + cdSize > size || cdSize > (256u << 20)) return false;
    Bytes cd;
    if (!readRange(diskPath, cdOff, cdSize, cd)) return false;
    int archive = (int)lib.archives.size();
    Archive a;
    a.path = diskPath;
    a.size = size;
    lib.archives.push_back(std::move(a));
    size_t p = 0;
    uint32_t added = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cd.size(); ++i) {
        const uint8_t* h = cd.data() + p;
        if (r32(h) != 0x02014b50) break;
        uint16_t method = (uint16_t)r16(h + 10);
        uint32_t comp = r32(h + 20), plain = r32(h + 24);
        uint32_t nameLen = r16(h + 28), extraLen = r16(h + 30), commentLen = r16(h + 32);
        uint32_t local = r32(h + 42);
        if (p + 46 + nameLen > cd.size()) break;
        std::string name((const char*)h + 46, nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.back() == '/') continue;
        if (method != 0 && method != 8) continue;          // nothing else is used
        // NFS Edge's apk carries its whole game as one stored FMOBB file
        // (assets/obb/*.obb.png): open it as the folder it is
        if (method == 0 && comp == plain && comp > (1u << 20)) {
            Bytes lh;
            if (readRange(diskPath, local, 30, lh) && lh[0] == 'P' && lh[1] == 'K') {
                size_t data0 = (size_t)local + 30 + r16(lh.data() + 26) + r16(lh.data() + 28);
                Bytes magic;
                if (readRange(diskPath, data0, 8, magic) && memcmp(magic.data(), "FMOBB-0", 7) == 0) {
                    std::string pre = relPrefix.empty() ? name : relPrefix + "/" + name;
                    if (indexFmobbAt(diskPath, data0, comp, pre, lib)) { ++added; continue; }
                }
            }
        }
        LibraryEntry e;
        e.path = relPrefix.empty() ? name : relPrefix + "/" + name;
        e.kind = classifyByName(e.path);
        e.archive = archive;
        e.offset = local;
        e.length = plain;
        e.packedLength = comp;
        e.zipMethod = method;
        e.zipped = true;
        lib.entries.push_back(std::move(e));
        ++added;
    }
    return added > 0;
}

// ------------------------------------------------------------------ FMOBB
// NFS Edge ships its data as one "FMOBB-02" file (named *.obb.png, though it
// is no picture). The header holds where the index is and how long it is;
// the index is "ZBIN" + gzip around an SBIN v4 document of nested
// DirectoryEntry {name, directories, files} records whose FileEntry items
// say where each file's bytes are, and whether they are Brotli-packed.
// `base` is where the FMOBB starts in diskPath: 0 for a loose file, or the
// data offset of a stored entry when it sits inside the .apk.
static bool indexFmobbAt(const std::string& diskPath, uint64_t base, uint64_t size,
                         const std::string& relPrefix, Library& lib) {
    Bytes h;
    if (size < 24 || !readRange(diskPath, (size_t)base, 24, h) || memcmp(h.data(), "FMOBB-0", 7) != 0)
        return false;
    uint64_t idxOff, idxLen;
    memcpy(&idxOff, h.data() + 8, 8);
    memcpy(&idxLen, h.data() + 16, 8);
    if (idxOff + idxLen > size || idxLen < 8 || idxLen > (64u << 20)) {
        lib.warnings.push_back(baseName(diskPath) + ": the FMOBB index is out of range");
        return false;
    }
    Bytes raw, sbin;
    if (!readRange(diskPath, (size_t)(base + idxOff), (size_t)idxLen, raw)) return false;
    if (memcmp(raw.data(), "ZBIN", 4) == 0) {
        if (!inflateGzip(raw.data() + 4, raw.size() - 4, sbin)) {
            lib.warnings.push_back(baseName(diskPath) + ": the FMOBB index did not unpack");
            return false;
        }
    } else {
        sbin.swap(raw);
    }
    auto chunks = sbinChunks(sbin.data(), sbin.size());
    const SbinChunk* stru = findChunk(chunks, "STRU");
    const SbinChunk* fiel = findChunk(chunks, "FIEL");
    const SbinChunk* ohdr = findChunk(chunks, "OHDR");
    const SbinChunk* data = findChunk(chunks, "DATA");
    if (!stru || !fiel || !ohdr || !data) return false;
    std::vector<std::string> names = sbinNames(chunks);
    auto rd16 = [&](size_t o) -> uint32_t {
        if (o + 2 > data->size) return 0;
        uint16_t v; memcpy(&v, data->data + o, 2); return v;
    };
    auto rd32 = [&](size_t o) -> uint32_t {
        if (o + 4 > data->size) return 0xFFFFFFFFu;
        uint32_t v; memcpy(&v, data->data + o, 4); return v;
    };
    auto nameOf = [&](uint32_t i) { return i < names.size() ? names[i] : std::string(); };
    // the structures and their field offsets, looked up by name
    struct Layout { int dirName = -1, dirDirs = -1, dirFiles = -1, dirSize = 12;
                    int fOff = -1, fUnc = -1, fComp = -1, fName = -1, fFlags = -1, fSize = 24;
                    int dirSid = -1, fileSid = -1; } L;
    for (size_t sid = 0; (sid + 1) * 6 <= stru->size; ++sid) {
        uint16_t n, first, count;
        memcpy(&n, stru->data + sid * 6, 2);
        memcpy(&first, stru->data + sid * 6 + 2, 2);
        memcpy(&count, stru->data + sid * 6 + 4, 2);
        std::string sn = nameOf(n);
        int end = 0;
        for (size_t f = first; f < (size_t)first + count && (f + 1) * 8 <= fiel->size; ++f) {
            uint16_t fn, ft, fo;
            memcpy(&fn, fiel->data + f * 8, 2);
            memcpy(&ft, fiel->data + f * 8 + 2, 2);
            memcpy(&fo, fiel->data + f * 8 + 4, 2);
            std::string nm = nameOf(fn);
            int sz = ft == 8 ? 8 : 4;
            end = std::max(end, (int)fo + sz);
            if (sn == "DirectoryEntry") {
                if (nm == "name") L.dirName = fo;
                else if (nm == "directories") L.dirDirs = fo;
                else if (nm == "files") L.dirFiles = fo;
            } else if (sn == "FileEntry") {
                if (nm == "offset") L.fOff = fo;
                else if (nm == "uncompressed_size") L.fUnc = fo;
                else if (nm == "compressed_size") L.fComp = fo;
                else if (nm == "name") L.fName = fo;
                else if (nm == "flags") L.fFlags = fo;
            }
        }
        if (sn == "DirectoryEntry") { L.dirSid = (int)sid; L.dirSize = end; }
        if (sn == "FileEntry") { L.fileSid = (int)sid; L.fSize = (end + 7) & ~7; }
    }
    if (L.dirSid < 0 || L.fileSid < 0 || L.fOff < 0 || L.fName < 0 || L.dirName < 0) {
        lib.warnings.push_back(baseName(diskPath) + ": the FMOBB index has an unknown layout");
        return false;
    }
    size_t nObj = ohdr->size / 4;
    auto object = [&](uint32_t i, uint32_t& kind, size_t& off) {
        if (i >= nObj) return false;
        uint32_t e; memcpy(&e, ohdr->data + (size_t)i * 4, 4);
        kind = e & 7; off = e >> 3;
        return off < data->size;
    };
    // an array of inline structures: u16 element type (0x10), u16 structure,
    // u32 count, then the elements back to back
    auto inlineArray = [&](uint32_t obj, int wantSid, size_t& first, uint32_t& count) {
        uint32_t k; size_t off;
        if (!object(obj, k, off) || k != 2) return false;
        if (rd16(off) != 0x10 || (int)rd16(off + 2) != wantSid) return false;
        count = rd32(off + 4);
        first = off + 8;
        return count < 10000000;
    };

    int archive = (int)lib.archives.size();
    Archive a;
    a.path = diskPath;
    a.size = (size_t)(base + size);
    lib.archives.push_back(std::move(a));
    uint32_t added = 0, far4GB = 0;

    // the root directory is the typed object the root map points at
    uint32_t rootObj = 1;
    {
        uint32_t k; size_t off;
        if (object(0, k, off) && k == 1) {
            uint32_t n = rd16(off);
            for (uint32_t e = 0; e < n && e < 64; ++e) {
                size_t at = off + 4 + (size_t)e * 12;
                if (nameOf(rd16(at)) == "root") { rootObj = rd32(off + rd16(at + 4)); break; }
            }
        }
    }
    struct Dir { size_t fields; std::string path; int depth; };
    std::vector<Dir> stack;
    {
        uint32_t k; size_t off;
        if (!object(rootObj, k, off) || k != 0) return false;
        stack.push_back({ off + 2, std::string(), 0 });
    }
    while (!stack.empty()) {
        Dir d = stack.back();
        stack.pop_back();
        if (d.depth > 64) continue;
        uint32_t filesObj = L.dirFiles >= 0 ? rd32(d.fields + L.dirFiles) : 0xFFFFFFFFu;
        uint32_t dirsObj = L.dirDirs >= 0 ? rd32(d.fields + L.dirDirs) : 0xFFFFFFFFu;
        size_t first; uint32_t count;
        if (filesObj != 0xFFFFFFFFu && inlineArray(filesObj, L.fileSid, first, count)) {
            for (uint32_t i = 0; i < count; ++i) {
                size_t f = first + (size_t)i * L.fSize;
                if (f + L.fSize > data->size) break;
                uint64_t off64;
                memcpy(&off64, data->data + f + L.fOff, 8);
                uint32_t unc = L.fUnc >= 0 ? rd32(f + L.fUnc) : 0;
                uint32_t comp = L.fComp >= 0 ? rd32(f + L.fComp) : unc;
                uint32_t flags = L.fFlags >= 0 ? rd32(f + L.fFlags) : 0;
                std::string nm = nameOf(rd32(f + L.fName));
                if (nm.empty() || off64 + comp > size) continue;
                uint64_t abs = base + off64;
                if (abs > 0xFFFFFFFFull) { ++far4GB; continue; }
                LibraryEntry e;
                std::string rel = d.path.empty() ? nm : d.path + "/" + nm;
                e.path = relPrefix.empty() ? rel : relPrefix + "/" + rel;
                e.kind = classifyByName(e.path);
                e.archive = archive;
                e.offset = (uint32_t)abs;
                e.length = unc;
                e.packedLength = comp;
                e.fmobb = true;
                e.brotli = (flags & 4) != 0 || comp != unc;
                e.fmobbFlags = flags;
                lib.entries.push_back(std::move(e));
                ++added;
            }
        }
        if (dirsObj != 0xFFFFFFFFu && inlineArray(dirsObj, L.dirSid, first, count)) {
            for (uint32_t i = count; i-- > 0;) {
                size_t f = first + (size_t)i * L.dirSize;
                if (f + L.dirSize > data->size) continue;
                std::string nm = nameOf(rd32(f + L.dirName));
                stack.push_back({ f, d.path.empty() ? nm : d.path + "/" + nm, d.depth + 1 });
            }
        }
    }
    if (far4GB)
        lib.warnings.push_back(baseName(diskPath) + ": " + std::to_string(far4GB) +
                               " file(s) lie past 4 GB and were left out");
    return added > 0;
}

bool isFmobbFile(const std::string& diskPath) {
    Bytes h;
    return readRange(diskPath, 0, 8, h) && memcmp(h.data(), "FMOBB-0", 7) == 0;
}

bool indexFmobb(const std::string& diskPath, const std::string& relPrefix, Library& lib) {
    return indexFmobbAt(diskPath, 0, fileSizeOf(diskPath), relPrefix, lib);
}

bool indexLooseFile(const std::string& diskPath, const std::string& relPath,
                    Library& lib, size_t sizeOnDisk) {
    // an Android expansion file or a zip is a folder of its own
    std::string ext0 = extensionOf(relPath);
    // NFS Edge's data file: FMOBB-02, whatever its name says
    if ((ext0 == "png" || ext0 == "obb") && isFmobbFile(diskPath) &&
        indexFmobb(diskPath, relPath, lib))
        return true;
    // an Apple .ipa (Real Racing, Real Racing GTI) is a zip as well
    if ((ext0 == "obb" || ext0 == "zip" || ext0 == "apk" || ext0 == "ipa") &&
        indexZipArchive(diskPath, relPath, lib))
        return true;
    LibraryEntry e;
    e.path = relPath;
    e.whole = true;
    e.archive = (int)lib.archives.size();
    e.length = (uint32_t)(sizeOnDisk ? sizeOnDisk : fileSizeOf(diskPath));

    // "foo.etc.dds.z" is a texture called foo.etc.dds; "foo.etc.dds.z.bin" is
    // the same thing as a run of frames. Naming the entry for the payload is
    // what makes the tree readable and what lets every other part of the tool
    // recognise the asset by its extension.
    std::string ext = extensionOf(e.path);
    if (ext == "bin") {
        std::string inner = stripExtension(e.path);
        if (extensionOf(inner) == "z") { e.path = inner; e.rr3Wrapped = true; ext = "z"; }
    }
    if (extensionOf(e.path) == "z") {
        e.path = stripExtension(e.path);
        e.rr3Wrapped = true;
    }
    e.kind = classifyByName(e.path);

    Archive a;
    a.path = diskPath;
    a.size = e.length;
    lib.archives.push_back(std::move(a));
    lib.entries.push_back(std::move(e));
    return true;
}

// Keep the most recently used cabinets; a car's model and its textures
// usually share one, so this turns a folder full of clicks into a single
// decompression.
void Library::trimCache() {
    while (cacheBytes > cacheLimit && !cacheOrder.empty()) {
        uint64_t oldest = cacheOrder.front();
        cacheOrder.erase(cacheOrder.begin());
        auto it = cache.find(oldest);
        if (it != cache.end()) {
            cacheBytes -= it->second.size();
            cache.erase(it);
        }
    }
}

bool Library::read(size_t entryIndex, Bytes& out, std::string* error) {
    out.clear();
    if (entryIndex >= entries.size()) {
        if (error) *error = "no such asset";
        return false;
    }
    const LibraryEntry& e = entries[entryIndex];
    if (e.archive < 0 || (size_t)e.archive >= archives.size()) {
        if (error) *error = "asset has no archive";
        return false;
    }
    Archive& arch = archives[e.archive];
    if (e.zipped) {
        Bytes lh;
        if (!readRange(arch.path, e.offset, 30, lh) || lh[0] != 'P' || lh[1] != 'K') {
            if (error) *error = "the zip entry's header is missing";
            return false;
        }
        size_t skip = 30 + (size_t)(lh[26] | (lh[27] << 8)) + (size_t)(lh[28] | (lh[29] << 8));
        Bytes packed;
        if (!readRange(arch.path, (size_t)e.offset + skip, e.packedLength, packed)) {
            if (error) *error = "the zip entry runs past the end of the file";
            return false;
        }
        if (e.zipMethod == 0) { out.swap(packed); return true; }
        if (!inflateRaw(packed.data(), packed.size(), out)) {
            if (error) *error = "the zip entry did not inflate";
            return false;
        }
        return true;
    }
    if (e.fmobb) {
        Bytes packed;
        if (!readRange(arch.path, e.offset, e.packedLength, packed)) {
            if (error) *error = "the file runs past the end of the FMOBB archive";
            return false;
        }
        if (!e.brotli) { out.swap(packed); return true; }
        // FileEntry.flags says the codec: 2 is LZHAM (No Limits VR, a
        // dictionary-size byte and seven more ahead of the stream), 1 deflate,
        // 4 Brotli (Edge). The others are tried when the stated one fails.
        auto sized = [&]() { return out.size() == e.length || !e.length; };
        auto tryLzham = [&]() {
            if (!lzhamAvailable()) return false;
            return lzhamDecompress(packed.data(), packed.size(), e.length, out) && sized();
        };
        auto tryDeflate = [&]() {
            out.clear();
            if (inflateZlib(packed.data(), packed.size(), out) && sized()) return true;
            out.clear();
            return inflateRaw(packed.data(), packed.size(), out) && sized();
        };
        auto tryBrotli = [&]() {
            out.clear();
            return brotliDecompress(packed.data(), packed.size(), e.length, out) && sized();
        };
        uint32_t f = e.fmobbFlags;
        if (f == 2 && tryLzham()) return true;
        if (f == 1 && tryDeflate()) return true;
        if ((f & 4) && tryBrotli()) return true;
        if (f != 2 && tryLzham()) return true;
        if (f != 1 && tryDeflate()) return true;
        if (!(f & 4) && tryBrotli()) return true;
        if (error) {
            if (f == 2 && !lzhamAvailable())
                *error = "this file is LZHAM-packed and the LZHAM decoder did not start";
            else
                *error = "the packed file did not unpack (FMOBB flags " + std::to_string(f) + ")";
        }
        return false;
    }
    if (e.whole) {
        if (!readFile(arch.path, out)) {
            if (error) *error = "cannot read the file";
            return false;
        }
        if (e.rr3Wrapped) {
            Bytes plain;
            std::string inner;
            if (!rr3Unwrap(baseName(arch.path), out.data(), out.size(), plain,
                           &inner, nullptr)) {
                if (error) *error = "the zlib wrapper did not decode";
                return false;
            }
            out.swap(plain);
        }
        return true;
    }

    if (e.cabinet >= arch.cabinets.size()) {
        if (error) *error = "cabinet not present (external or DLC archive)";
        return false;
    }

    uint64_t key = ((uint64_t)(uint32_t)e.archive << 32) | e.cabinet;
    auto it = cache.find(key);
    if (it == cache.end() && (arch.cabinets[e.cabinet].flags & CAB_EXTERNAL)) {
        // Not in the .pack: <folder of the pack>\<pack name>\<index>.cab, the
        // same place QuickBMS's unpacker opens. Stored as it is when 0x20 is
        // set or 4 is not; zstd otherwise.
        const Cabinet& cab = arch.cabinets[e.cabinet];
        std::string packPath = arch.path;
        size_t slash = packPath.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? std::string() : packPath.substr(0, slash + 1);
        std::string stem = stripExtension(baseName(packPath));
        char leaf[32];
        snprintf(leaf, sizeof(leaf), "%u.cab", (unsigned)e.cabinet);
#ifdef _WIN32
        std::string cabPath = dir + stem + "\\" + leaf;
#else
        std::string cabPath = dir + stem + "/" + leaf;
#endif
        if (cab.length > (512u << 20)) {
            if (error) *error = "cabinet size is implausible - manifest damaged";
            return false;
        }
        Bytes packed;
        if (!readFile(cabPath, packed) || packed.empty()) {
            if (error) *error = "external cabinet missing: " + stem + "\\" + leaf;
            return false;
        }
        Bytes blob;
        if ((cab.flags & CAB_STORED) || !(cab.flags & CAB_ZSTD)) {
            blob.swap(packed);
        } else {
            if (!zstdAvailable()) {
                if (error) *error = "Zstandard cabinet - libzstd not found";
                return false;
            }
            if (!zstdDecompress(packed.data(), packed.size(), cab.length, blob)) {
                if (error) *error = "zstd decode failed (external cabinet)";
                return false;
            }
        }
        cacheBytes += blob.size();
        cache[key] = std::move(blob);
        cacheOrder.push_back(key);
        trimCache();
        it = cache.find(key);
        if (it == cache.end()) {
            if (error) *error = "external cabinet larger than the cache";
            return false;
        }
    }
    if (it == cache.end()) {
        const Cabinet& cab = arch.cabinets[e.cabinet];
        // Refuse a cabinet whose record does not fit the file it claims to be
        // in. Without this a damaged or misread manifest asks for a several
        // gigabyte allocation, which ends the program rather than the read.
        if (arch.size &&
            (size_t)cab.packedOffset + cab.packedLength + arch.dataBase > arch.size) {
            if (error) *error = "cabinet range outside the archive";
            return false;
        }
        if (cab.length > (512u << 20) || cab.packedLength > (512u << 20)) {
            if (error) *error = "cabinet size is implausible - manifest damaged";
            return false;
        }
        Bytes packed;
        if (!readRange(arch.path, arch.dataBase + cab.packedOffset,
                       cab.packedLength, packed)) {
            if (error) *error = "cabinet range outside the archive";
            return false;
        }
        Bytes blob;
        std::string err;
        if (!decompressCabinetBlock(packed.data(), packed.size(), cab, blob, err)) {
            if (error) *error = err;
            return false;
        }
        cacheBytes += blob.size();
        cache[key] = std::move(blob);
        cacheOrder.push_back(key);
        trimCache();
        it = cache.find(key);
        if (it == cache.end()) {       // larger on its own than the whole cache
            Bytes blob2;
            std::string err2;
            if (!decompressCabinetBlock(packed.data(), packed.size(), cab, blob2, err2)) {
                if (error) *error = err2;
                return false;
            }
            if ((size_t)e.offset + e.length > blob2.size()) {
                if (error) *error = "asset outside its cabinet";
                return false;
            }
            out.assign(blob2.begin() + e.offset, blob2.begin() + e.offset + e.length);
            return true;
        }
    } else {
        // touch: move to the back of the order
        auto pos = std::find(cacheOrder.begin(), cacheOrder.end(), key);
        if (pos != cacheOrder.end()) {
            cacheOrder.erase(pos);
            cacheOrder.push_back(key);
        }
    }

    const Bytes& blob = it->second;
    if ((size_t)e.offset + e.length > blob.size()) {
        if (error) *error = "asset outside its cabinet";
        return false;
    }
    out.assign(blob.begin() + e.offset, blob.begin() + e.offset + e.length);
    return true;
}

void Library::clear() {
    archives.clear();
    entries.clear();
    warnings.clear();
    cache.clear();
    cacheOrder.clear();
    cacheBytes = 0;
}

} // namespace nfsnl
