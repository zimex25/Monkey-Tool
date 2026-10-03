// nfsnl.h - Need for Speed: No Limits asset library
//
// Portable C++17. No external dependencies except an optional Zstandard
// library which is loaded dynamically at runtime (libzstd.dll / libzstd.so).
//
// Handles:
//   .pack  - PACK/ZBDS archive with an AssetPack manifest and cabinets
//   .cab   - bare SBIN blob
//   .sba   - texture asset (PNG/JPEG payloads, or raw pixels)
//   .sb3d  - model asset (vertex/index buffers + scene description)
//
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <functional>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(_MSC_VER)
// 64-bit file positions (_fseeki64 / _ftelli64) come from the Windows C
// runtime's stdio.h; this is only for the syntax check's stand-in headers
extern "C" int _fseeki64(FILE*, long long, int);
extern "C" long long _ftelli64(FILE*);
#endif

namespace nfsnl {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- utility
// fopen for a UTF-8 path, on Windows as well
FILE* openFile(const std::string& path, const char* mode);
bool readFile(const std::string& path, Bytes& out);
bool writeFile(const std::string& path, const uint8_t* data, size_t len);
bool writeFile(const std::string& path, const Bytes& data);
void makeDirsFor(const std::string& filePath);
std::string joinPath(const std::string& a, const std::string& b);
std::string extensionOf(const std::string& path);
std::string stripExtension(const std::string& path);
std::string baseName(const std::string& path);

// ------------------------------------------------------------- inflate
// Raw DEFLATE (RFC1951) and gzip (RFC1952) decoders, written here so the
// library has no zlib dependency.
bool inflateRaw(const uint8_t* src, size_t srcLen, Bytes& out, size_t* consumed = nullptr);
bool inflateZlib(const uint8_t* src, size_t srcLen, Bytes& out, size_t* consumed = nullptr);
bool inflateGzip(const uint8_t* src, size_t srcLen, Bytes& out, size_t* consumed = nullptr);
// and the other way: fixed-Huffman DEFLATE with LZ77 matching, wrapped as
// zlib (RFC1950) or gzip. Smaller than stored blocks by a wide margin, and
// read back by anything - the game's own loaders included.
Bytes deflateRaw(const uint8_t* src, size_t len);
Bytes zlibCompress(const uint8_t* src, size_t len);
Bytes gzipCompress(const uint8_t* src, size_t len);
uint32_t crc32Of(const uint8_t* src, size_t len);

// ------------------------------------------------------------- zstandard
// Loaded at runtime; absent simply means zstd-compressed content is skipped.
bool zstdAvailable();
std::string zstdBackend();
bool zstdDecompress(const uint8_t* src, size_t srcLen, size_t expectedSize, Bytes& out);

// ------------------------------------------------------------- LZHAM
// The codec the game compresses its models with, loaded at runtime like
// Zstandard. Absent simply means compressed .m3g models are reported rather
// than converted.
bool lzhamAvailable();
// the LZHAM decoder compiled into the program (src/lzham)
int lzhamBuiltinDecompress(uint32_t dictLog2, uint32_t updateRate, uint32_t flags,
                           uint8_t* dst, size_t* dstLen, const uint8_t* src, size_t srcLen,
                           uint32_t* adler);
std::string lzhamBackend();
bool lzhamSetPath(const std::string& path);
bool lzhamDecompress(const uint8_t* src, size_t srcLen, size_t expectedSize, Bytes& out);

// ------------------------------------------------------------- SBIN
struct SbinChunk {
    char tag[5];
    const uint8_t* data;
    size_t size;
};
std::vector<SbinChunk> sbinChunks(const uint8_t* data, size_t len);
// LZ4 block (no frame header); `expected` is the unpacked size (0 = unknown)
bool lz4DecompressBlock(const uint8_t* src, size_t n, size_t expected, Bytes& out);
// Brotli (RFC 7932): NFS Edge's FMOBB archives pack their files with it
bool brotliDecompress(const uint8_t* src, size_t n, size_t expected, Bytes& out);
const SbinChunk* findChunk(const std::vector<SbinChunk>& chunks, const char* tag);
std::vector<std::string> sbinNames(const std::vector<SbinChunk>& chunks);

// ------------------------------------------------------------- PACK
enum CabinetFlags {
    CAB_DEFLATE = 1,
    CAB_LZHAM = 2,
    CAB_ZSTD = 4,
    CAB_ENCRYPTED = 8 | 16,
    // The data has been stored as it is - QuickBMS's reading, and it only
    // matters for external cabinets, which are otherwise zstd when 4 is set.
    CAB_STORED = 0x20,
    // The cabinet is not in the .pack at all: it is <pack name>\<index>.cab
    // in a folder beside it.
    CAB_EXTERNAL = 0x40,
};

struct Cabinet {
    uint32_t flags = 0, length = 0, packedOffset = 0, packedLength = 0;
};
struct PackFile {
    uint32_t nameIndex = 0, cabinetIndex = 0, offset = 0, length = 0;
};
struct PackFolder {
    uint32_t nameIndex = 0, filesIndex = 0, filesCount = 0,
             foldersIndex = 0, foldersCount = 0;
};

struct PackManifest {
    std::vector<Cabinet> cabinets;
    std::vector<PackFile> files;
    std::vector<PackFolder> folders;
    std::vector<std::string> names;
    size_t dataBase = 0;
    std::string variant;     // sku tag such as "2x" or "texture_etc"
    bool valid = false;

    // file index -> full in-game path
    std::map<uint32_t, std::string> resolvePaths() const;
};

// One extracted asset held in memory.
struct Asset {
    std::string path;    // in-game path, e.g. models/cars/x/x.sb3d
    Bytes data;
    std::string kind;    // "texture" | "model" | "sound" | "other"
};

struct ExtractResult {
    std::vector<Asset> assets;
    std::vector<std::string> warnings;
    int blockedByCodec = 0;
};

// Extract every named asset from one archive file.
void extractArchive(const std::string& path, ExtractResult& result);

size_t fileSizeOf(const std::string& path);
PackManifest parsePackManifest(const Bytes& meta, size_t packSize, size_t dataBase);
bool decompressCabinetBlock(const uint8_t* raw, size_t rawLen, const Cabinet& cab,
                            Bytes& out, std::string& err);

// ------------------------------------------------------------- library
// Where an asset lives, rather than the asset itself. Indexing an archive
// reads its manifest only; the bytes are produced when something asks for
// them, so nothing is ever written to disk.
struct LibraryEntry {
    std::string path;      // in-game path, with the sku prefix
    std::string kind;      // texture | model | sound | other
    int archive = -1;
    uint32_t cabinet = 0;
    uint32_t offset = 0, length = 0;
    bool whole = false;    // the archive is itself the asset (a bare .cab)
    // Real Racing 3 keeps its files loose on disk rather than in archives, and
    // wraps almost all of them in zlib. The entry is named for what comes out
    // of the wrapper; this says the wrapper has to come off on the way.
    bool rr3Wrapped = false;
    // an entry of a .zip / .obb: `offset` is its local header, `length` the
    // unpacked size, `packedLength` what is stored; method 0 stored, 8 deflate
    bool zipped = false;
    uint16_t zipMethod = 0;
    uint32_t packedLength = 0;
    // an entry of an FMOBB archive (NFS Edge): `offset` is where its bytes
    // start in the file, `packedLength` how many there are; brotli says they
    // are a Brotli stream that unpacks to `length` bytes
    bool fmobb = false;
    bool brotli = false;
    // FMOBB: how the file is packed - 0 stored, 1 deflate, 2 LZHAM (No Limits
    // VR), 4 Brotli (Edge)
    uint32_t fmobbFlags = 0;
};

struct Archive {
    std::string path;
    size_t dataBase = 0;
    size_t size = 0;          // the archive's size on disk, for range checks
    std::vector<Cabinet> cabinets;
};

struct Library {
    std::vector<Archive> archives;
    std::vector<LibraryEntry> entries;
    std::vector<std::string> warnings;

    // decompressed cabinets, most recently used last
    std::map<uint64_t, Bytes> cache;
    std::vector<uint64_t> cacheOrder;
    size_t cacheBytes = 0;
    size_t cacheLimit = 192u * 1024 * 1024;

    bool read(size_t entryIndex, Bytes& out, std::string* error = nullptr);
    void clear();
    void trimCache();
};

// Read one archive's manifest and add everything it holds to the library.
bool indexArchive(const std::string& path, Library& lib);

// Add one loose file - Real Racing 3's .depot holds thousands of them rather
// than any archive. `relPath` is how it should read in the tree; the container
// extensions are stripped from it and taken off the bytes on the way out.
// every file inside a .zip / .obb / .apk, listed as its own asset under
// `relPrefix` (Android games ship their data this way: Most Wanted's .obb)
bool indexZipArchive(const std::string& diskPath, const std::string& relPrefix, Library& lib);
// NFS Edge's "FMOBB-02" data file (main.*.obb.png)
bool isFmobbFile(const std::string& diskPath);
bool indexFmobb(const std::string& diskPath, const std::string& relPrefix, Library& lib);
bool indexLooseFile(const std::string& diskPath, const std::string& relPath,
                    Library& lib, size_t sizeOnDisk = 0);

// ------------------------------------------------------------- game profiles
// What the tool needs to know about one game. Monkey Tool opens with a
// profile picker, so support for another of the studio's games is a profile
// plus whatever format readers it needs - not a second program.
struct GameProfile {
    std::string id;             // "nfs_no_limits"
    std::string name;           // "Need for Speed: No Limits"
    std::string publisher;
    std::string folderHint;     // the folder the user is asked to point at
    std::string note;           // shown under the name in the picker
    std::vector<std::string> archiveExtensions;   // ".pack", ".cab"
    bool supported = true;
    int logoResource = 0;       // RCDATA id of the logo, 0 for none
    int iconResource = 0;       // the picker's app icon (PNG)
    int splashResource = 0;     // the loading screen's art (JPEG)
    int pickerRow = 0, pickerColumn = 0;   // where it sits in the picker
    // Real Racing 3 has no archives at all: .depot is a folder of loose files,
    // most of them zlib-wrapped. Indexing walks the folder instead of reading
    // manifests.
    bool looseFiles = false;
    // where the game's own files start under the folder the user picks
    std::vector<std::string> folderSteps;
};

const std::vector<GameProfile>& gameProfiles();
const GameProfile* findProfile(const std::string& id);
// Which game a folder holds, from its path (the Android package names are
// distinctive) and, failing that, from names found inside it. `names` is a
// sample of the file and folder names under it, lower case. Empty when it
// cannot tell.
std::string detectGameFolder(const std::string& path, const std::vector<std::string>& names);

std::string classifyByName(const std::string& name);

// ------------------------------------------------------------- images
struct Image {
    int width = 0, height = 0, channels = 0;   // channels: 1,3,4
    Bytes pixels;                              // row-major, top-down
    bool ok() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// ImageFormatType, exactly as the .sba's own string table enumerates it. The
// file states which codec it holds, so nothing here has to be guessed.
enum SbaCodec {
    SBA_UNKNOWN = -1,
    SBA_DEFAULT = 0,
    SBA_RGB = 1,
    SBA_RGBA = 2,
    SBA_PVRTC_2BPP_RGB = 3,
    SBA_PVRTC_2BPP_RGBA = 4,
    SBA_PVRTC_4BPP_RGBA = 5,
    SBA_PVRTC_4BPP_RGB = 6,
    SBA_DXT1 = 7,
    SBA_DXT3 = 8,
    SBA_DXT5 = 9,
    SBA_ATC_RGB = 10,
    SBA_ATC_RGBA_EXPLICIT = 11,
    SBA_ATC_RGBA_INTERPOLATED = 12,
    SBA_ETC_RGB = 13,
    SBA_PNG = 14,
    SBA_JPEG = 15,
    // newer builds enumerate more; these are the ones that can be decoded
    SBA_ETC2_RGB = 100,
    SBA_ETC2_RGBA = 101,
    SBA_RGB565 = 102,
    // ASTC: 1000 + block width * 100 + block height (ASTC_LDR_6x6 = 1606)
    SBA_ASTC_BASE = 1000,
};
// ASTC LDR (Real Racing Next): `bw` x `bh` blocks of 16 bytes to RGBA
bool decodeAstc(const uint8_t* data, size_t len, int width, int height, int bw, int bh, Image& out);
const char* sbaCodecName(int codec);

struct SbaEntry {
    int index = 0;
    std::string format;   // "png" | "jpg" | "pvr" | "ktx" | "astc" | "dds" | "raw"
    Bytes data;
    int width = 0, height = 0;   // from the .sba metadata when known
    int codec = SBA_UNKNOWN;     // declared ImageFormatType
    // when the stored number fits two codecs of the same size (the Ford GT's
    // 6 is PVRTC_4BPP_RGB or DXT1 depending on the build), the other one:
    // the decoder tries both and keeps the picture that is not noise
    int codecAlt = SBA_UNKNOWN;
};

// Image blobs inside a .sba, largest first.
std::vector<SbaEntry> readSba(const uint8_t* data, size_t len);

// No Limits' UI texture packs (texturepacks/ui/*.sba): many named pictures
// ("FrontEnd/LTS/.../lts_card_back.png"), each a rectangle of one of the
// file's images (an atlas page). Empty when the file is not a TexturePack.
struct TexturePackBox {
    std::string name;
    int blob = -1, format = -1, imageW = 0, imageH = 0;   // the page it sits on
    int x = 0, y = 0, w = 0, h = 0;                       // source_rect on that page
};
std::vector<TexturePackBox> readTexturePack(const uint8_t* data, size_t len);
// every picture of a texture pack, cut out of its page (top-down RGBA)
struct NamedImage { std::string name; Image image; };
std::vector<NamedImage> texturePackPictures(const uint8_t* data, size_t len);

// Decoders / encoders
bool decodePng(const uint8_t* data, size_t len, Image& out);
// baseline / extended sequential JPEG (progressive goes through WIC)
bool decodeJpeg(const uint8_t* data, size_t len, Image& out);
// a picture a user brings: PNG, JPEG, and on Windows anything WIC reads;
// the tool's own texture formats too
bool decodeImageFile(const uint8_t* data, size_t len, Image& out);

// ------------------------------------------------------------- import
// Pixels to 8-bit RGBA, and a resize (area average down, linear up, alpha
// weighted so transparent edges keep their colour).
Image toRgba(const Image& src);
Image resizeRgba(const Image& src, int w, int h);
// block and pixel encoders; the input is RGBA as the container stores it
Bytes encodeEtc1(const Image& rgba);
Bytes encodeDxt(const Image& rgba, int variant);        // 1, 3 or 5
Bytes encodeAtc(const Image& rgba, int variant);        // 1 RGB, 3 explicit, 5 interpolated
Bytes encodeMasked(const Image& rgba, int bytesPerPixel, const uint32_t mask[4]);
// A picture into a texture file, keeping its size, codec and mip levels:
// .sba (SBIN 3 and 4), .pvr (2 and 3), .dds, .png, .jpg. `report` says what
// was done, or why nothing could be.
bool importPicture(const std::string& assetPath, const Bytes& original, const Image& picture,
                   Bytes& out, std::string& report);
// the .pvr / .dds part of it; `img` in the container's own orientation
bool importIntoContainer(const Bytes& file, const Image& img, Bytes& out, std::string& report,
                         bool unused = false);
// Real Racing 3's .z wrapper: the plain size, then a zlib stream
Bytes rr3WrapZ(const Bytes& plain);

// A complete PVR file (version 3 "PVR\3", or the legacy version 2 with
// "PVR!" at offset 44). Handles PVRTC 2bpp/4bpp, ETC and DXT payloads.
bool decodePvrContainer(const uint8_t* data, size_t len, Image& out);

// A complete DDS file. Real Racing 3 ships its textures this way: a standard
// 128-byte DDS header whose FourCC is "ETC " for ETC1 blocks, or zero for
// uncompressed RGBA4444. DXT FourCCs are handled too, so a DDS from anywhere
// else opens as well.
bool decodeDdsContainer(const uint8_t* data, size_t len, Image& out);

// Turn an image upside down in place.
//
// The game stores its textures bottom-up, the way OpenGL wants them. Up to
// 0.61 the tool showed them exactly as stored, which is why a car card was
// previewed - and saved - upside down. Everything decoded here is flipped once
// at the point of decoding, so previews, saved images and the textures the 3D
// viewer samples all agree; the model readers are told not to flip V to match.
void flipImageVertically(Image& img);

// ETC1 / ETC2 / DXT1 / DXT5 / PVRTC block data. `hint` may contain the pack sku
// (e.g. "texture_etc"); with no hint both codecs are tried and the least
// noisy result is kept.
bool decodeBlockCompressed(const uint8_t* data, size_t len, int w, int h,
                           const std::string& hint, Image& out);

// png, raw pixels, or block-compressed payloads
bool decodeImageAuto(const SbaEntry& e, Image& out, const std::string& hint = "");
// Any texture file the tool knows: .sba, a PVR (v2 or v3) or a DDS.
bool decodeTextureFile(const uint8_t* data, size_t len, Image& out);
// The names a model's texture reference may have on disk, best first: the
// path without its leading ../, and - since NFS Edge's .m3g models still
// name "texture_x.m3g" while the build ships texture_x.sba - the same with
// the texture extensions the games use.
std::vector<std::string> textureRefCandidates(const std::string& ref);
// NFS Edge (ETC1 has no alpha) keeps a texture's alpha in a second one beside
// it: texture_x.sba + texture_x_ETCAlpha.sba. The companion's name, and the
// merge: its grey level becomes the colour texture's alpha.
std::string etcAlphaCompanion(const std::string& path);
// every file that may hold a texture's alpha: <name>_ETCAlpha (NFS Edge),
// <name>_alpha.<ext> (Real Racing 3 tracks)
std::vector<std::string> alphaCompanions(const std::string& path);
bool applyEtcAlpha(Image& color, const Image& alpha);

Bytes encodePng(const Image& img);
Bytes encodeBmp(const Image& img);
Bytes encodeTga(const Image& img);
Bytes encodeJpeg(const Image& img, int quality = 90);
Bytes makeDds(int w, int h, const char fourcc[4], const uint8_t* payload, size_t len);

// Convert one .sba to a chosen output format. `format` is one of
// png/jpg/bmp/tga/dds/raw. Returns the encoded bytes.
bool convertSba(const uint8_t* data, size_t len, const std::string& format, Bytes& out,
                std::string* usedExtension, std::string* error,
                const std::string& hint = "");

// ------------------------------------------------------------- models
struct Mesh {
    std::string name;
    std::vector<float> positions;   // xyz triples
    std::vector<float> normals;     // xyz triples
    std::vector<float> uvs;         // uv pairs (may be empty)
    // Vertex colours, RGBA in 0..1, four per vertex (empty when the mesh has
    // none). Artists use this channel for things a texture cannot carry per
    // mesh: baked ambient occlusion and dirt, which window glass is tinted,
    // where the paint colour applies. The game shaders multiply it in, so a
    // mesh exported without it can look flat or wrongly coloured.
    std::vector<float> colors;
    std::vector<float> uvs2;        // second uv set (No Limits paint: livery layout)
    std::vector<uint32_t> indices;  // triangle list
    std::string material;           // material name as the file spells it
    std::string texture;            // diffuse texture path, as stated in the file
    float color[4] = {1, 1, 1, 1};  // the material's diffuse colour
    std::string lod;                // "LOD00" ..., by detail rather than by name
    std::string part;               // the part it belongs to, e.g. bumper_rear_a
    // No Limits body kits: the same slot (bumper_front, spoiler, hood) comes
    // in several versions, told apart by a letter - a is the car as it ships,
    // the others are kits and tuning parts. kit is that letter, '+' for an
    // add-on with no letter (overfenders, ducktail), 0 for a part every
    // version shares.
    std::string kitSlot;
    char kit = 0;
    // a part moved by one of the model's animations (Real Racing 3 rear
    // wings): which animation and which of its nodes; -1 when it does not move
    int animIndex = -1, animNode = -1;
    // the bounds the file states for this mesh; two body kits of the same
    // part differ here, the detail levels of one kit do not
    float bboxMin[3] = {0, 0, 0}, bboxMax[3] = {0, 0, 0};
    // where the group the mesh hangs from sits in the file (NFS Undercover /
    // Shift track pieces: a layout places the piece, not the file)
    float origin[3] = {0, 0, 0};
};

// A material as the game describes it. The name encodes how the engine draws
// it - opaque / alpha / alphaadd, twosided, nozwrite, layerNN - and those are
// render states a viewer can reproduce without any of the game's shaders.
struct Material {
    std::string name;
    std::string diffuse;    // path as the model states it
    std::string normal;     // sibling ..._normal texture, when one exists
    std::string specular;   // sibling ..._reflection texture, when one exists
    bool alphaBlend = false;
    bool additive = false;
    bool twoSided = false;
    bool noDepthWrite = false;
    int layer = 0;          // draw order the name asks for, 0 when unstated
    // DiffuseColor from the file: what an untextured material (black gloss
    // paint, bronze chrome) is drawn in
    float color[4] = {1, 1, 1, 1};
    bool hasColor = false;
};

// A named place on a car that carries no geometry of its own: where a wheel
// goes, where the steering wheel sits, which way a mirror folds. Real Racing 3
// keeps these in a .points file beside the model, and several of the model's
// own parts - the steering wheel, the rev-counter needle - are modelled at the
// origin and belong at one of these. Without them the parts pile up in the
// middle of the car.
struct Hardpoint {
    std::string name;       // POINT_WHEEL_FL, HINGE_MIRROR_LEFT, ...
    float pos[3] = {0, 0, 0};   // already converted into the model's own space
    bool hinge = false;         // a hinge also states an orientation
    float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major 3x3
};

// A moving part's animation (Real Racing 3 .banim): each node has seven
// keyed channels - lateral, fore, up (1/65536 of a unit) and three rotations
// (1/65536 of a turn) - as (time ms, value) keys over `duration` ms.
struct PartAnim {
    std::string name;             // "wing"
    float duration = 0;
    struct Node { std::vector<std::pair<float, float>> ch[7]; };
    std::vector<Node> nodes;
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<PartAnim> anims;
    std::vector<std::string> warnings;
    // every material the file declares, whether or not a mesh uses it
    std::vector<Material> materials;
    // hardpoints that came with the model, exported as empties
    std::vector<Hardpoint> points;
    // textures the file carries inside it (an M3G track's atlases); a mesh
    // names one as "#img:<index>"
    std::vector<Image> images;
    bool valid = false;
};

// The game stores textures bottom-up and this reader now flips them at decode
// time, so the default is to leave V alone. Passing true restores the 0.61
// behaviour for anyone who wants it.
Model loadSb3d(const uint8_t* data, size_t len, bool flipV = false);

// M3G (JSR-184 Mobile 3D Graphics), used by older builds of the game.
bool isM3g(const uint8_t* data, size_t len);

// True when the file is wrapped in the game's own "DA BD" container, which
// holds a compressed M3G. Reports how large the contents are.
bool m3gWrapperInfo(const uint8_t* data, size_t len, size_t* uncompressedSize);
// No Limits' "DA BD" (LZHAM) and EA's gzip wrappers taken off; false when the
// data is not wrapped (or, with `error` set, when unwrapping failed)
bool m3gUnwrap(const uint8_t* data, size_t len, Bytes& out, std::string* error = nullptr);
Model loadM3g(const uint8_t* data, size_t len, bool flipV = false);
// Standard JSR-184 scenes (NFS Undercover, Shift, Shift 2): the node tree's
// transforms applied, the textures a file carries decoded into images
Model loadJsr184Scene(const uint8_t* data, size_t len);
// a car_<name>.m3g of those games: the body kit's parts as kit B, the shop's
// add-ons as kit +, the shadow card hidden, and the car's two textures
// (texture_car_<name>.m3g, ..._bodykit.m3g) bound; `find` looks a file up by
// name and returns its path, "" when there is none
int eaPhoneOrganizeCar(Model& car, const std::string& carName,
                       const std::function<std::string(const std::string&)>& find);
// a Shift 2 track (no textures inside): the location's tracktex atlases by blending
int eaPhoneTextureTrack(Model& track, const std::string& trackLeaf,
                        const std::function<std::string(const std::string&)>& find);
// a Shift 2 track by its texturelist_<track>.bin and every other track's
// list (`leaves`: the file names in the game folder; `read` fetches a path)
int eaPhoneTextureTrackLists(Model& track, const std::string& trackLeaf, const std::vector<std::string>& leaves,
                             const std::function<std::string(const std::string&)>& find,
                             const std::function<bool(const std::string&, Bytes&)>& read);
// everything the game binds to an NFS Undercover / Shift / Shift 2 model by
// its name: a car's textures and wheels, a cockpit's textures, a location's
// layout and textures. `layout` picks the layout file (else the largest).
struct EaPhoneFiles {
    std::vector<std::string> leaves;                              // file names in the game folder
    std::function<std::string(const std::string&)> find;          // file name -> path, "" if none
    std::function<bool(const std::string&, Bytes&)> read;         // path -> bytes
};
std::vector<std::string> eaPhoneDress(Model& m, const std::string& stem, const EaPhoneFiles& files,
                                      const std::string& layout = std::string());
std::vector<std::string> eaPhoneLayoutCandidates(const std::string& stem, const std::vector<std::string>& leaves);
int eaPhoneAttachWheels(Model& car, const Model& wheels, const std::function<std::string(const std::string&)>& find);
int eaPhoneOrganizeInterior(Model& m, const std::string& stem, const std::function<std::string(const std::string&)>& find);
// an event's layout (<location>_<event>.bin) of a location's road pieces
struct EaTile { float x = 0, z = 0; int rot = 0; int mirror = 0;   // bit 0 mirrors x, bit 1 z
                int kind = 0; std::vector<int> pieces; };
bool eaPhoneReadLayout(const uint8_t* data, size_t len, std::vector<EaTile>& tiles);
// the pieces placed as the layout says (positions times `unit`); returns how many
int eaPhoneLayoutTrack(Model& track, const std::vector<EaTile>& tiles, float unit = 10.0f);
// an M3G Image2D's pixels (formats 99/100 bytes, 124/125 EA's PVRTC 4 bpp)
bool decodeM3gPixels(int format, int w, int h, const uint8_t* px, size_t n, Image& out);
// PVRTC 4/2 bpp blocks, as a .pvr holds them
bool decodePvrtcBlocks(const uint8_t* data, size_t len, int w, int h, bool twoBpp, Image& out);

// Picks the right reader from the file's magic.
Model loadModel(const uint8_t* data, size_t len, bool flipV = false);
// 1 JSR184 (Real Racing 3), 2 IM2M3G (Most Wanted 2012), 3 IM3M3G,
// 4 IM4M3G (No Limits, after unwrapping); 0 when the file has no identifier
int identifierVersionOf(const uint8_t* data, size_t len);

// ------------------------------------------------------- Real Racing 3
//
// A different game, a different set of containers, and - confusingly - a
// different ".m3g". No Limits uses that extension for its own "DA BD" wrapper
// around an LZHAM-compressed blob; Real Racing 3 uses it for a genuine
// JSR-184 Mobile 3D Graphics file, which is why the same extension needs two
// readers. The magic tells them apart, so nothing has to be guessed.

// "<uint32 uncompressed size><zlib stream>" - the .z files in .depot.
bool rr3InflateZ(const uint8_t* data, size_t len, Bytes& out);

// A .z.bin is a chain of those blocks, each a complete file of its own. Car
// shadows ship this way: one frame per sun angle, every frame the same size.
bool rr3BinFrames(const uint8_t* data, size_t len, std::vector<Bytes>& out,
                  size_t maxFrames = 0);

// True when the name or the bytes say this is one of the two containers.
bool rr3IsContainer(const std::string& name, const uint8_t* data, size_t len);

// Unwrap either container to the payload (the first frame, for a .bin) and
// report the name the payload should carry. Anything else is passed through.
bool rr3Unwrap(const std::string& name, const uint8_t* data, size_t len,
               Bytes& out, std::string* innerName, size_t* frameCount);

// The .points sidecar: hardpoints in the file's own units, converted here into
// the same space as the model's vertices.
std::vector<Hardpoint> rr3ReadPoints(const uint8_t* data, size_t len);

// ---- .nct, a car's data file ----
// One fixed pad XORed over the whole file, the same pad for every car. The
// transform is its own inverse, so this both decodes and re-encodes.
size_t nctPadLength();
size_t nctPadKnownBytes();
bool nctPadByteKnown(size_t offset);   // pinned, rather than a best guess

// Finish the pad from a whole game's worth of .nct files - one per car - and
// keep the result. Returns false, and says why, when the corpus is too small
// or disagrees with the bytes already pinned by known plaintext.
bool nctLearnPad(const std::vector<Bytes>& files, std::string& report);
// The same with each file's name alongside: the names are clues in their own
// right (a liveries file carries its car's name at byte 4, a car file its model
// year at byte 12) and say which files are XML, which are struct data and
// which are texture lists. useShipped=false ignores the pad built into the
// tool - only for testing the learner against a pad it has never seen.
bool nctLearnPad(const std::vector<Bytes>& files, const std::vector<std::string>& names,
                 std::string& report, bool useShipped = true);
bool nctPadSave(const std::string& path);
bool nctPadLoad(const std::string& path);
bool nctTransform(const uint8_t* data, size_t len, Bytes& out,
                  size_t* covered = nullptr);
std::string nctText(const std::string& name, const uint8_t* data, size_t len);
// Encode an edited, decoded file back (the same XOR). `uncertain` counts the
// offsets the pad is not certain of; `pastPad` the bytes beyond its end, which
// are copied unchanged. False when some bytes could not be encoded.
bool nctEncode(const uint8_t* plain, size_t len, Bytes& out,
               size_t* uncertain = nullptr, size_t* pastPad = nullptr);
// A decoded <car>.liveries.bin: the car's name and its texture table.
struct NctLiveries {
    struct Texture { uint32_t id = 0; size_t offset = 0; std::string path; bool sure = false; };
    bool ok = false;
    uint32_t header = 0, count = 0;
    std::string car;
    size_t tableStart = 0, tableEnd = 0;
    std::vector<Texture> textures;
};
NctLiveries nctReadLiveries(const uint8_t* plain, size_t len);

// A genuine JSR-184 / Firemint M3G model.
bool isJsr184(const uint8_t* data, size_t len);
Model loadJsr184(const uint8_t* data, size_t len, bool flipV = false);

// ---- data files as editable text (nfsnl_data.cpp) ----
// The text an editor shows for a data file: plain text as it is, a .sounddef
// by its fields, other binary tables as a reversible listing. False (and why)
// for a file that is encrypted.
bool dataEditableText(const std::string& path, const uint8_t* data, size_t len,
                      std::string& text, std::string& why);
// That text, edited, back into the file's own format.
bool dataFromEditedText(const std::string& path, const std::string& text,
                        const uint8_t* original, size_t originalLen,
                        Bytes& out, std::string& why);
std::string dataToText(const uint8_t* data, size_t len);
bool dataFromText(const std::string& text, Bytes& out, std::string& why);
bool dataIsEncrypted(const uint8_t* data, size_t len);
// the .wav files a .sounddef plays
std::vector<std::string> soundDefSamples(const uint8_t* data, size_t len);

// ---- model import (nfsnl_import.cpp) ----
// An OBJ or FBX (binary or ASCII) read into meshes, one per object, named as
// the file names them.
Model readImportModel(const uint8_t* data, size_t len, const std::string& name);
// Where the viewer put a mesh: its centre as the file has it, how far the
// .points placement moved it, and its largest extent.
struct MeshPlacement { float centre[3] = {0, 0, 0}, offset[3] = {0, 0, 0}; float size = 0; };
// A Real Racing 3 .m3g with the named meshes' geometry replaced by the
// imported model's (and meshes the file lacks added, cloned from one of the
// same material). `report` says what happened either way.
std::map<std::string, MeshPlacement> meshPlacements(const Model& raw, const Model& placed);
// A whole new car .m3g from an OBJ / FBX, on the frame of an existing Real
// Racing 3 car model (header, materials, mesh layout; its meshes left out).
bool rr3BuildCar(const Bytes& templateM3g, const Model& imported, Bytes& out, std::string& report);
// Part and material names of the model the car does not have, each with the
// car's own name it most likely means (same spelling but for case and
// spaces; else the same _mm_<atlas> and the nearest part name).
struct NameFix { bool material = false; std::string from, to; };
std::vector<NameFix> rr3SuggestNames(const Bytes& m3g, const Model& model, bool newCar);
void rr3ApplyNameFixes(Model& model, const std::vector<NameFix>& fixes);
bool rr3ReplaceMeshes(const Bytes& original, const Model& imported,
                      const std::map<std::string, MeshPlacement>& placement,
                      Bytes& out, std::string& report);
// IM2M3G (NFS Most Wanted 2012) and IM4M3G (NFS No Limits, once unwrapped):
// version is the digit in the identifier. See nfsnl_im2.cpp.
Model loadImM3g(const uint8_t* data, size_t len, int version, bool flipV = false);
// Most Wanted cars with stand-in pictures: their textures by the car's name
int mwFillCarTextures(Model& m, const std::string& car);
// No Limits' track geometry: prefabs/tracks/<region>.scene_static.sba, an
// SBIN of Instances, Primitives and vertex buffers. See nfsnl_scene.cpp.
bool isNlScene(const uint8_t* data, size_t len);
Model loadNlScene(const uint8_t* data, size_t len);
// textures for a scene's parts, by material name, from the candidates given
// (lower-case file stem, asset path). Returns how many parts got one.
// The textures a No Limits track's parts use, from the region's
// .lightmaps.sba: entry N's diffuse path for the scene's "#materialvars#N".
std::vector<std::string> nlReadMaterialVars(const uint8_t* data, size_t len);
int nlApplyMaterialVars(Model& m, const std::vector<std::string>& vars,
                        const std::function<std::string(const std::string&)>& find);
int nlAssignSceneTextures(Model& m, const std::string& scenePath,
                          const std::vector<std::pair<std::string, std::string>>& textures);
// A region's limited-time decorations (Halloween, Christmas, an event's
// finale): in <region>.scene.sb, top-level actors holding a LayerScene - a
// layer of extra static geometry in its own scene file - and models the
// actors place (NFSModel). One group per such actor.
struct NlPlacedModel { std::string file; float matrix[16]; };   // column-major, metres
struct NlLtsGroup {
    std::string name;                       // lts_2024_paradyne_end, halloween_2024_hopebridge_lts
    std::vector<std::string> layerFiles;    // LayerScene FileName: Halloween_2024
    std::vector<std::string> layerNames;    // layer_halloween_2024, halloween_2024_hopebridge_lts_layer
    std::vector<NlPlacedModel> models;
};
std::vector<NlLtsGroup> nlSceneLtsGroups(const uint8_t* sceneSbin, size_t n);
// a group's name for people: lts_2024_paradyne_end -> "2024 paradyne end"
std::string nlLtsLabel(const NlLtsGroup& g);
// a model moved by a placement matrix (positions, normals, bounds)
void transformModel(Model& m, const float matrix[16]);
// A texture stored as an M3G file (Hot Pursuit's texture_*.m3g): its Image2D.
bool decodeM3gImage(const uint8_t* data, size_t len, Image& out);
// one line per object: its type, its size and the names it carries
std::string m3gObjectMap(const uint8_t* data, size_t len);

// Move the parts that are modelled at the origin onto the hardpoints they
// belong to - the steering wheel, the needles, the driver's hands - and record
// the points on the model so they survive the export. Parts already placed in
// the car's own space are left exactly where they are.
void rr3PlaceParts(Model& m, const std::vector<Hardpoint>& points);
// The exterior .points of many cars has no POINT_STEERING_WHEEL, although the
// exterior model has a steering wheel (seen through the windows), modelled at
// the origin. The cockpit's <car>_int.points has it, in the same frame: this
// copies the cockpit points the exterior lacks (steering wheel, needles,
// gearstick) across. rr3InteriorPointsNameFor gives that file's name.
void rr3MergeCockpitPoints(std::vector<Hardpoint>& exterior, const std::vector<Hardpoint>& interior);
std::string rr3InteriorPointsNameFor(const std::string& modelPath);
// The car's wheel corners: <car>_shared.m3g beside <car>_a.m3g ("" if none)
std::string rr3SharedNameFor(const std::string& modelPath);

// A part that moves - a rear wing - is modelled at the origin and its rest
// position lives in the animation that moves it, not in the .points file.
// Reads that rest position out of a .banim.
bool rr3ReadBanimRest(const uint8_t* data, size_t len, float out[3],
                      std::string* note = nullptr);

// Move the parts whose name contains `partHint` onto that rest position, but
// only those still sitting at the origin, and only if the position lands
// somewhere on the model.
// Real Racing 3 driver_lod_*.m3g: its .banim beside it, and its fifteen parts
// posed from that animation (00_helmet ... 14_rightleg_foot = nodes 0-14).
std::string rr3DriverAnimNameFor(const std::string& modelPath);
bool rr3PoseDriver(Model& m, const uint8_t* banim, size_t len);
void rr3PlaceAnimated(Model& m, const uint8_t* banim, size_t banimLen,
                      const std::string& partHint);
// The whole .banim: every node, every keyed channel.
bool rr3ReadBanim(const uint8_t* data, size_t len, PartAnim& out, std::string* note = nullptr);
// Where a moving part is at time t (ms), relative to where it rests (t = 0):
// a 4x4 column-major matrix, identity for a part that does not move.
void partAnimDelta(const Model& m, const Mesh& mesh, float t, float out[16]);
// The longest animation the model has, in ms (0 when nothing moves)
float modelAnimDuration(const Model& m);
// Put every moving part where it is at time t (for a still frame or export)
void poseModel(Model& m, float t);

// Add four copies of a wheel model at POINT_WHEEL_*, each scaled so its
// radius matches the height that point sits at. Returns how many were placed.
// No Limits: a car's wheels are a separate model, models/cars/wheels/
// wheel_<car>.sb3d, placed by the game on the car's brake discs. This puts one
// copy on each corner the car has a mesh_rotor_<corner> for. Returns the number
// of corners placed.
// How the car's stock wheel arch fits the wheel, per axle ([0] front, [1]
// rear), from data/visualparts/C_<car>_1.sb (WheelArchUpgrade): the tyre is
// widened by TireWidthOffset (the Beck Kustoms F132 runs +6 cm front, +11 cm
// rear on one wheel model) and the radius scaled by WheelRadiusScale.
struct NlWheelTweak {
    float widthOffset[2] = { 0, 0 };
    float radiusScale[2] = { 1, 1 };
    float profileOffset[2] = { 0, 0 };
    float wheelOffset[2] = { 0, 0 };     // the whole wheel further out (m)
    bool valid = false;
};
bool nlVisualPartsWheelTweak(const uint8_t* sbin, size_t n, NlWheelTweak& out);
// does a visualparts file name (C_Acura_NSX_Type_S_2022_1.sb) belong to the car id
// (acura_nsx_type_s_2022)? The words must match, in any order.
bool nlVisualPartsMatches(const std::string& visualPartsFile, const std::string& carId);
// No Limits rim paint: the game's rim colours (data/colours/colours.sb, the
// CG_*_RIM groups) multiply the parts of the wheel texture its *_rim_tint
// meshes use; the logos and centre caps (*_notint) keep their own colours.
struct NlRimColour { std::string id, name, group; float rgb[3] = { 1, 1, 1 }; };
std::vector<NlRimColour> nlRimColours(const uint8_t* sbin, size_t n);
// Every paint the game offers (data/colours/colours.sb): body, rim, brake
// caliper and window colours, each with the hash the car setups use.
enum NlPaintKind { NL_BODY = 0, NL_RIM = 1, NL_BRAKE = 2, NL_WINDOW = 3, NL_PAINT_KINDS = 4 };
struct NlPaintColour {
    std::string id, name, group, finish;   // V_BC_GT3_Orange_Gloss, "GT3 Orange Gloss", CG_COMMON_BODY, Gloss
    int kind = -1;
    uint32_t hash = 0;                      // HashedID
    float rgb[3] = { 1, 1, 1 };
    bool matte = false;
};
std::vector<NlPaintColour> nlPaintColours(const uint8_t* sbin, size_t n);
// A car setup (data/car_setups/<car>/Bodykits/<X>_STOCK.sb, .../Customs/*.sb)
// lists the hashes of what the car wears; the colours among them are its paint.
struct NlCarPaint {
    bool have[NL_PAINT_KINDS] = { false, false, false, false };
    float rgb[NL_PAINT_KINDS][3] = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
    std::string name[NL_PAINT_KINDS];
    bool any() const { return have[0] || have[1] || have[2]; }
};
NlCarPaint nlCarSetupPaint(const uint8_t* setupSbin, size_t n, const std::vector<NlPaintColour>& colours);
// A painted texture is named "#paint:<mode>:<rrggbb>:<texture>": mode 1 the
// texture's grey texels times the colour (rims, calipers), mode 2 the colour
// alone (the body, which the game draws from its paint shader, not a texture).
std::string nlPaintRef(int mode, const float rgb[3], const std::string& base);
bool nlParsePaintRef(const std::string& ref, int& mode, float rgb[3], std::string& base);
std::string nlUnpaintRef(const std::string& ref);
void nlPaintImage(Image& img, int mode, const float rgb[3]);
// the car's paint on its body, rim (the wheel's *_tint parts) and caliper
// meshes; returns how many meshes took it. nlRemoveCarPaint undoes it.
int nlApplyCarPaint(Model& m, const NlCarPaint& paint);
void nlRemoveCarPaint(Model& m);
// the texels of `textureLeaf` (texture_wheel_x.sba) that the wheel's tinted
// meshes cover, w x h, 255 = painted; false when no tinted mesh uses it
bool nlRimTintMask(const Model& wheel, const std::string& textureLeaf, int w, int h,
                   std::vector<uint8_t>& mask);
// the paint: each masked texel times the colour (an empty mask: every grey
// texel, the colourful logos left alone)
void nlTintImage(Image& img, const std::vector<uint8_t>& mask, const float rgb[3]);
int nlAttachWheels(Model& car, const Model& wheel, const float* axleRadius = nullptr,
                   const NlWheelTweak* tweak = nullptr);
// the tyre radius per axle from a car's decoded prefab SBIN ([0] front, [1] rear)
bool nlPrefabWheelRadii(const uint8_t* d, size_t n, float radius[2]);
// the models a car's prefab hangs on it (a cop car's light bar), each with
// its place on the car (the actors' translations added up)
struct PrefabModel { std::string file; float pos[3] = { 0, 0, 0 }; };
std::vector<PrefabModel> nlPrefabModels(const uint8_t* sbin, size_t n);
// the wheel model a No Limits car uses, by its path: models/cars/x/x.sb3d ->
// "wheel_x.sb3d"
std::string nlWheelNameFor(const std::string& carPath);
// Real Racing 1 / GTI: a .rr_car record (the car's files) and the model's
// fixed mesh order turned into LODs, a cockpit group, the steering wheel on
// its point and the textures the record names
struct Rr1Car {
    std::string name, model, wheelModel, wheelTexture, points, exterior, interior, steering;
    std::vector<std::string> extra;
};
bool rr1ReadCar(const uint8_t* d, size_t n, Rr1Car& car);
bool rr1IsCar(const Model& m);
void rr1OrganizeCar(Model& m, const std::vector<Hardpoint>& pts, const Rr1Car* car);
// insetToPoint: the point is the wheel's outer face (Real Racing 1 / GTI),
// so the wheel goes inward from it by its width; otherwise the hub is on it
int rr3AttachWheels(Model& car, const Model& wheel,
                    const std::vector<Hardpoint>& points, bool insetToPoint = false);

std::string writeObjString(const Model& m);

// Write an already-assembled model - used when the caller has added parts of
// its own, such as wheels pulled in from another file.
bool writeModel(const Model& m, const std::string& format, Bytes& out);
std::string writeFbxString(const Model& m);

// Convert one model to "fbx" or "obj". When the game ships a .points sidecar
// beside the model, pass it: parts modelled at the origin are placed and the
// hardpoints are exported as empties.
bool convertSb3d(const uint8_t* data, size_t len, const std::string& format,
                 Bytes& out, std::string* error,
                 const uint8_t* pointsData = nullptr, size_t pointsLen = 0);

// The .points file that belongs to a model: strip the extension, drop a
// trailing single-letter detail suffix ("_a"), add ".points". So
// cars/1979_porsche_935_a.m3g asks for cars/1979_porsche_935.points.
std::string rr3PointsNameFor(const std::string& modelPath);
// Real Racing 2 models name no textures; the game pairs them by file name:
// car_<name>_int.m3g uses car_<name>_int.pvr, its steering wheel _sw.pvr, the
// hood and mirror seen from inside the paint (_ext_01.pvr); an outside model
// takes _ext_01.pvr, with _wheel, _wheel_blur, _cab and _sha for those parts.
// Fills in mesh.texture where the model left it empty.
void rr2AssignTextures(Model& m, const std::string& modelPath);
// Real Racing 3's models name no texture files: a material is called
// "Vehicle Exterior_mm_misc" and the texture is the car's own
// <car>_misc.etc.dds. This fills Mesh::texture and the material list from
// that rule. find() is asked for a file stem ("1979_porsche_935_misc", or a
// prefix ending in '*') and returns the asset path it found, or "" - so the
// viewer, the export and the command line each look in their own place.
// Returns the number of meshes that got a texture.
// common lists the shared textures (vehicles/common/...) as pairs of
// lower-case file stem and asset path; liveries is the car's decoded
// <car>.liveries.bin, which names the default paint and the shared textures
// the car uses (either may be empty). report gets the _mm_ names that found
// no texture.
// a texture's folder, as rr3FolderTexture files it: the parent folder's name
// (lower case), or <object>/billboarded
std::string rr3FolderKey(const std::string& path);
// a track object's picture by the folder it is filed in, when the file
// itself has a sibling's name (byFolder: rr3FolderKey -> path)
std::string rr3FolderTexture(const std::string& stem, const std::multimap<std::string, std::string>& byFolder);
int rr3AssignTextures(Model& m, const std::string& modelPath,
                      const std::function<std::string(const std::string&)>& find,
                      const std::vector<std::pair<std::string, std::string>>& common,
                      const NctLiveries* liveries = nullptr, std::string* report = nullptr);

// ------------------------------------------------------------- text
// A readable dump of one SBIN file: its chunks, the structures and enums it
// declares, and every name it holds.
std::string sbinText(const uint8_t* d, size_t len);
// SBIN chunk hashes: FNV-1 (32-bit) of each chunk's data
uint32_t sbinFnv1(const uint8_t* d, size_t n);
void sbinRehash(Bytes& sbin);
// the objects in an SBIN file as indented text (version 3 and 4), with a
// check of every chunk's hash; stops after about `limit` characters
std::string sbinObjectsText(const uint8_t* d, size_t len, size_t limit = 4u << 20);
// the same, with an object several fields share written out at each of them
// (not thread-safe: for readers that parse the text)
std::string sbinObjectsTextExpanded(const uint8_t* d, size_t len, size_t limit = 64u << 20);
// Most Wanted 2012's (SBIN version 3) field type names, nullptr if unknown
const char* sbinVersion3TypeName(int t);
struct SbinImageRecord { int blob = -1, width = 0, height = 0; std::string format; };
// the mip levels a Most Wanted 2012 .sba (SBIN version 3) declares
std::vector<SbinImageRecord> sbinVersion3Images(const uint8_t* d, size_t len);
// No Limits .sb data files: most are encrypted (the save cipher, IV from the
// file name) around "ZBDS" + size + gzip. Plain SBIN passes through.
bool nlSbDecode(const std::string& fileName, const uint8_t* d, size_t n, Bytes& sbin,
                std::string* how = nullptr);
// and back: rehashes, gzips, wraps in ZBDS and encrypts for `fileName`
Bytes nlSbEncode(const std::string& fileName, const Bytes& sbin);

// The same for any asset - SBIN, bank, sound, or a hex listing when the file
// has no schema to show.
std::string assetText(const std::string& assetPath, const uint8_t* d, size_t len);

// ------------------------------------------------------------- audio
// One Wwise sound, as its RIFF header describes it.
struct WemInfo {
    uint16_t codec = 0;          // 0xFFFF = Wwise Vorbis, 1 = PCM, 2 = IMA ADPCM
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    uint32_t fmtSize = 0;
    size_t   dataOffset = 0;
    size_t   dataSize = 0;
    uint32_t totalSamples = 0;
    uint32_t setupPacketOffset = 0;        // Vorbis only
    uint32_t firstAudioPacketOffset = 0;   // Vorbis only
    uint8_t  blockSize0 = 0, blockSize1 = 0;
    bool     inlineSetup = false;          // false = codebooks are external
};

bool readWemInfo(const uint8_t* d, size_t len, WemInfo& info);
const char* wemCodecName(int codec);

// PCM / float / IMA ADPCM are decoded here. Wwise Vorbis is reported, with
// the reason, rather than returned wrong.
bool wemToWav(const uint8_t* d, size_t len, Bytes& out, std::string* error);
Bytes makeWav(const int16_t* samples, size_t frameCount, int channels, int sampleRate);

// The granular engine sounds ("Gnsu20"): a bank of grains plus the rev range
// they cover, which the game crossfades as the engine speed changes.
struct GnsuInfo {
    std::string version;
    float minRpm = 0, maxRpm = 0;
    uint32_t grainCount = 0, entryCount = 0, totalSamples = 0, sampleRate = 0;
};
bool readGnsuInfo(const uint8_t* d, size_t len, GnsuInfo& g);
// The whole grain run as one mono wav (EA-XAS v0 decoded here).
bool gnsuToWav(const uint8_t* d, size_t len, Bytes& out, std::string* error = nullptr);
// EA SPS stream (Real Racing 2): header facts, and the audio as a wav when
// the codec is EA-XAS v1 or PCM (others go through vgmstream)
bool spsInfo(const uint8_t* d, size_t len, int* codec, int* channels, int* rate, uint32_t* samples);
// FMOD sample banks (.fsb, FSB5: Most Wanted, NFS Edge): the names of the
// sounds inside, in order (vgmstream's subsong N is names[N-1])
std::vector<std::string> fsb5Names(const uint8_t* d, size_t len);
// an FMOD Designer project (.fev): its events and the wave files it names
std::vector<std::string> fevStrings(const uint8_t* d, size_t len);
bool spsToWav(const uint8_t* d, size_t len, Bytes& out, std::string* error = nullptr);

struct BankEntry {
    uint32_t id = 0;
    size_t offset = 0;      // absolute, inside the bank
    size_t size = 0;
    std::string kind;       // "wem" | "gnsu" | "bin"
    uint16_t codec = 0, channels = 0;
    uint32_t sampleRate = 0, samples = 0;
    float minRpm = 0, maxRpm = 0;
};

struct Bank {
    uint32_t id = 0, version = 0, hircObjects = 0;
    std::vector<std::string> sections;
    std::vector<BankEntry> entries;
    // every media id the bank's Sounds and Music Tracks play, embedded or not
    std::vector<uint32_t> sources;
};

// DIDX/DATA is an exact table of what the bank contains, so splitting one
// never has to guess.
bool readBank(const uint8_t* d, size_t len, Bank& bank);
std::string describeBank(const Bank& bank);

// ------------------------------------------------------------- viewer
// Everything the 3D view needs to draw a frame. The camera orbits a fixed
// point; the model itself is moved with modelPos, which is the XYZ offset the
// viewer's position boxes edit.
struct RenderView {
    float yaw = 0.6f;          // radians, orbit around the vertical axis
    float pitch = 0.25f;       // radians, positive looks down at the model
    float distance = 5.0f;     // camera distance from the orbit centre
    float pan[2] = {0, 0};     // screen-space slide of the camera
    float modelPos[3] = {0, 0, 0};   // XYZ the user moves the model by
    float centre[3] = {0, 0, 0};     // orbit centre, set by frameModel
    float modelSize = 1.0f;    // largest bounding-box side, for sane step sizes
    float radius = 1.0f;       // bounding-sphere radius, what the framing fits
    float fovY = 0.9f;         // vertical field of view, radians
    float background[3] = {38, 40, 46};
    // Draw back faces too. The game's panels are single-sided shells whose
    // winding is not consistent, so culling them leaves holes and the car
    // looks see-through - which is what it did in 0.4.
    bool twoSided = true;
    bool textured = true;      // false shades in plain grey, like a viewport
    bool hideGlow = true;      // skip the additive light-glow cards
    bool vertexColors = false; // multiply in the meshes' vertex colour (baked AO)
    std::string lodFilter;     // draw only this LOD; empty draws all of them
    // which body kit to draw: "stock", "all", or one kit letter ("y")
    std::string kit = "stock";
    // the Frosty-style stage: a sky gradient behind, a floor grid under the
    // model (at its lowest point)
    bool sky = true;
    bool floor = true;
    // a soft shadow of the model on the floor (cars and props; left off for
    // whole tracks, which are their own ground)
    bool shadow = true;
};

// Which meshes a view draws: LOD filter, body kit, hidden glow cards.
std::vector<uint8_t> visibleMeshes(const Model& m, const RenderView& view);
// The camera's axes for a view (right, up, forward), as the renderers use them.
void viewAxes(const RenderView& view, float right[3], float up[3], float fwd[3]);
// Height of the floor: the lowest point of what is drawn, model offset included.
float floorHeight(const Model& m, const RenderView& view);
// A grid spacing that reads well for a model of this size (1, 2 or 5 x 10^n).
float gridStep(float radius);
// Sky colours, top and horizon, 0..255 RGB
void skyColours(float top[3], float horizon[3]);
// See-through parts. A texture's alpha: the share of fully clear and of
// part-clear pixels (4-channel images only).
struct AlphaStats { float clear = 0, mid = 0; bool has = false; };
AlphaStats alphaStats(const Image& img);
enum AlphaMode { ALPHA_OPAQUE = 0, ALPHA_CUTOUT = 1, ALPHA_BLEND = 2 };
// How a part is drawn: solid, cut out where its texture is clear (badges,
// grilles), or blended (glass). From the material's own word where there is
// one (No Limits: _opaque / _alpha), else from the texture's alpha and the
// part's colour alpha. alphaScale: a constant opacity to multiply in.
AlphaMode meshAlphaMode(const Mesh& mesh, const AlphaStats* tex, float* alphaScale);
// Real Racing 3's material shaders are two lookup ramps each: fresnel_<x>
// (reflection by viewing angle) and spec_<x> (highlight by N.H), 1024 x 1
// .rgb.pvr files in shaders/. rr3ShaderFor says which one a part would use
// ("gloss", "chrome", "glass", "tires" ...; empty for a part that is not
// Real Racing 3's); rr3ShaderMatcap bakes a pair into a sphere map that a
// viewer adds on top of the textured part (GL_SPHERE_MAP texgen).
std::string rr3ShaderFor(const Mesh& mesh);
Image rr3ShaderMatcap(const Image* fresnel, const Image* spec, int size = 128);
// Whether the view casts the model's shadow: shadow and floor on, and a model
// the size of a car or a prop rather than a whole track.
bool wantsShadow(const RenderView& view);
// The shadow's light: unit vector towards it, nearly straight overhead.
// `jitter` 0..3 tilts it slightly, for the soft edge.
void shadowLight(int jitter, float L[3]);
// Bounds (in the model's own coordinates) of what the view draws.
bool modelFootprint(const Model& m, const RenderView& view, float lo[3], float hi[3]);

// Point the camera at the model: fills centre, distance and modelSize from
// the highest LOD's bounds. Call once when a model is opened.
void frameModel(const Model& m, RenderView& view);

// The kit letters a model has besides the stock 'a', sorted ("byz").
std::string modelKits(const Model& m);
// One flag per mesh: drawn in `kit` ("stock", "all" or a letter). A slot the
// chosen kit has no version of falls back to its stock version.
std::vector<uint8_t> kitMask(const Model& m, const std::string& kit);

// Draw the model into `out` (24-bit, top-down). `textures`, when given, holds
// one decoded diffuse image per mesh - a null entry means flat shading for
// that mesh.
void renderModel(const Model& model, const RenderView& view,
                 const std::vector<const Image*>* textures,
                 int width, int height, Image& out);

// ------------------------------------------------------------- top level
// What a given asset can be saved as; first entry is the default.
std::vector<std::string> formatsFor(const std::string& assetPath);
std::vector<std::string> rrNextTextureCandidates(const std::string& car, const std::string& material);
// Real Racing Next: the one wheel at the origin copied to the four corners
// its <car>_carpoints.sb gives (the wheel arches). Returns the corners placed.
int rrNextAttachWheels(Model& car, const uint8_t* carpoints, size_t len);

// Convert any asset to the requested format. For formats not applicable the
// original bytes are returned. `usedExtension` reports the extension the
// result actually is, which can differ from the request when a payload
// cannot be decoded (e.g. a compressed GPU texture falls back to .bin) -
// always name the output file with it rather than the requested format.
bool convertAsset(const std::string& assetPath, const uint8_t* data, size_t len,
                  const std::string& format, Bytes& out, std::string* error,
                  std::string* usedExtension = nullptr,
                  const uint8_t* sidecar = nullptr, size_t sidecarLen = 0);

} // namespace nfsnl
