// nfsnl_save.h - the Save Editor's core: No Limits and Real Racing 3 saves
//
// A port of the Firemonkeys Save Editor (fmsave, Go) into the tool itself, so
// it runs inside Monkey Tool's own window instead of as a second program.
// Nothing here touches the UI; main_win32.cpp draws the window.
#pragma once
#include "nfsnl.h"
#include <map>
#include <string>
#include <vector>

namespace nfsnl {
namespace saves {

// ---- primitives, exposed for the self-tests ----
void aes256CbcDecrypt(const uint8_t key[32], const uint8_t iv[16], uint8_t* buf, size_t len);
void aes256CbcEncrypt(const uint8_t key[32], const uint8_t iv[16], uint8_t* buf, size_t len);
void md5(const uint8_t* data, size_t len, uint8_t out[16]);
std::string base64(const uint8_t* data, size_t len);
uint32_t fnv1(const uint8_t* data, size_t len);
// Go's strconv.FormatFloat(f, 'g', -1, 32): the shortest text that reads back
// as the same float
std::string shortestFloat(float f);
// the No Limits cipher on its own, for data files as well as saves: AES-256-CBC
// with PKCS7, the IV made from the file name (no directory)
bool nfsDecryptAny(const std::string& name, const Bytes& c, Bytes& p, std::string& err);
Bytes nfsEncryptAny(const std::string& name, const Bytes& p);

// ================================================================ No Limits
//
//  file  = AES-256-CBC (PKCS7) of an SBIN v4 blob, one key for every player
//  iv    = the file name repeated to 16 bytes, XOR a fixed mask
//  SBIN  = chunks {tag, len, FNV-1 hash, data, pad to 4}
//  <uid>_m.sb, the manifest, holds base64(MD5) of every section's plaintext

struct NfsFile {
    std::string name, path;         // "489867946_1.sb", full path
    Bytes P;                        // decrypted SBIN
    struct Chunk { size_t hdr = 0, data = 0, len = 0; };
    std::map<std::string, Chunk> chunks;
    std::vector<std::string> strs;  // CHDR/CDAT string table
    std::vector<uint32_t> ohdr;
    std::string oldMd5;
    bool dirty = false;
};

struct NfsLeaf {
    int file = 0;
    std::string path;
    int type = 0;
    size_t off = 0;                 // absolute offset in the plaintext
};

struct NfsMain {
    const char* label;
    std::string path;
    std::string also;               // ledger that takes the same increase
};
const std::vector<NfsMain>& nfsMainFields();

class NfsSave {
public:
    std::string dir, uid;
    std::vector<NfsFile> files;
    bool hasManifest = false;
    NfsFile manifest;
    std::vector<NfsLeaf> leaves;
    std::map<std::string, size_t> byPath;

    bool load(const std::string& folder, std::string& err);
    std::string get(const NfsLeaf& l) const;
    bool editable(const NfsLeaf& l) const;
    bool set(const NfsLeaf& l, const std::string& text, std::string& err);
    const NfsLeaf* find(const std::string& path) const;
    bool setMain(const NfsMain& m, const std::string& text, std::string& err);
    // a text field (the player's name): a new string-table entry
    bool setString(const NfsLeaf& l, const std::string& text, std::string& err);
    // writes the changed sections and the manifest; the originals go to
    // <dir>/save_editor_backup_<stamp> first. Returns that folder.
    bool save(const std::string& stamp, std::string& backupDir, std::string& err);
    bool anyDirty() const;
};

// ================================================================ Real Racing 3
//
//  file   = plaintext XOR a repeating 64-byte key
//  plain  = header | dictionary | root object | object table | checksum
//  header = magic ABCFFCBA, version, total size, name count, key count (u32 BE)
//  the last byte is the XOR of every byte before it

struct Rr3Rec {
    uint32_t key = 0;
    std::string name;
    Bytes keyBytes;
    uint8_t tag = 0;
    Bytes raw;                      // the encoded value, tag included
    uint64_t u = 0;
    float f = 0;
    std::string s;
    Bytes blob;
    int type() const { return tag & 7; }
    std::string valueString() const;
    bool editable() const;
    bool setFromString(const std::string& text, std::string& err);
    void setUint(uint64_t v);
    void setBlob(const Bytes& b);
};

struct Rr3Hidden { const char* label; const char* field; const char* keyName; bool wide; };
const std::vector<Rr3Hidden>& rr3HiddenFields();
struct Rr3Plain { const char* label; const char* field; };
const std::vector<Rr3Plain>& rr3PlainFields();

class Rr3Save {
public:
    std::string path;
    Bytes plain;
    std::map<uint32_t, std::string> names;
    std::vector<Rr3Rec> root;
    std::map<std::string, size_t> byName;
    size_t rootFrom = 0, rootTo = 0;

    bool load(const std::string& file, std::string& err);
    bool parse(const std::string& file, const Bytes& raw, std::string& err);
    Bytes build() const;
    bool save(const std::string& stamp, std::string& backupDir, std::string& err);
    Rr3Rec* find(const std::string& name);

    bool hiddenGet(const Rr3Hidden& h, const std::map<std::string, uint64_t>& keys,
                   uint64_t& value) const;
    bool hiddenSet(const Rr3Hidden& h, const std::map<std::string, uint64_t>& keys,
                   uint64_t value, std::string& err);
    bool calibrate(const Rr3Hidden& h, std::map<std::string, uint64_t>& keys,
                   uint64_t shown, std::string& err);
    std::vector<size_t> sortedRoot() const;
private:
    bool hiddenStored(const Rr3Hidden& h, uint64_t& v) const;
};

bool rr3ParseNum(const std::string& text, uint64_t& v, std::string& err);

// keys made by Calibrate, as "name=hex" lines
std::map<std::string, uint64_t> loadKeys(const std::string& file);
bool saveKeys(const std::string& file, const std::map<std::string, uint64_t>& keys);

// Look under `root` (four folders deep) for a save: "rr3" with the folder of
// character.2.dat, or "nfs" with the folder of the *.sb files.
bool findSave(const std::string& root, std::string& game, std::string& dir);

// ---- the player's profile picture ----
// A picture kept in the save itself: a Real Racing 3 blob that holds a PNG
// or a JPEG (the name of the record is kept in `field`). -1 when there is none.
long rr3FindPicture(const Rr3Save& s, std::string* format = nullptr);
// Put `pic` in its place: scaled and cropped to the stored picture's size,
// written in its format.
bool rr3SetPicture(Rr3Save& s, size_t index, const Image& pic, std::string& report);
// The avatar the save names (a string field called ...avatar..., ...portrait...,
// ...profile_pic..., ...picture...), for either game; empty when none.
std::string nfsAvatarName(const NfsSave& s, std::string* field = nullptr);
std::string rr3AvatarName(const Rr3Save& s, std::string* field = nullptr);
// The player's name field: the leaf / record, or -1 / nullptr when the save
// has none.
long nfsPlayerNameLeaf(const NfsSave& s);
Rr3Rec* rr3PlayerName(Rr3Save& s);
// `pic` resized to w x h, cropped to fill (centre), RGBA
Image fitPicture(const Image& pic, int w, int h);

} // namespace saves
} // namespace nfsnl
