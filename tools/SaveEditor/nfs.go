package main

// Need for Speed No Limits save format (saves-encrypted/<uid>_N.sb, <uid>_m.sb)
//
//  file  = AES-256-CBC(PKCS7) of an SBIN v4 blob
//  key   = derived in libapp.so from "EEF6770F04E347ce9F48C22CAD77856D" and
//          "NFS2014MarmosetDefaultFontPage" (constant for every player)
//  iv    = file name repeated to 16 bytes XOR 031425364758697a8b9cadbecfe0f102
//  SBIN  = "SBIN" v4 + chunks {tag, len, FNV-1 hash, data, pad4}
//  manifest (_m.sb) stores base64(MD5(plaintext)) of every section file

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/md5"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

var nfsKey = []byte{
	0xca, 0x3f, 0x71, 0x6e, 0x03, 0x58, 0x7a, 0x78, 0x63, 0x0e, 0x4b, 0xed, 0xdd, 0xf4, 0xee, 0xdc,
	0x92, 0xb8, 0x9d, 0x9c, 0xa6, 0x76, 0x44, 0x71, 0x4f, 0x7e, 0x32, 0x2b, 0x25, 0x55, 0x40, 0xb9,
}
var nfsIVMask = []byte{0x03, 0x14, 0x25, 0x36, 0x47, 0x58, 0x69, 0x7a, 0x8b, 0x9c, 0xad, 0xbe, 0xcf, 0xe0, 0xf1, 0x02}

func nfsIV(name string) []byte {
	iv := make([]byte, 16)
	for i := range iv {
		iv[i] = name[i%len(name)] ^ nfsIVMask[i]
	}
	return iv
}

func nfsDecrypt(name string, c []byte) ([]byte, error) {
	if len(c) == 0 || len(c)%16 != 0 {
		return nil, errors.New("size is not a multiple of 16")
	}
	b, _ := aes.NewCipher(nfsKey)
	p := make([]byte, len(c))
	cipher.NewCBCDecrypter(b, nfsIV(name)).CryptBlocks(p, c)
	n := int(p[len(p)-1])
	if n < 1 || n > 16 || !bytes.Equal(p[len(p)-n:], bytes.Repeat([]byte{byte(n)}, n)) {
		return nil, errors.New("wrong key or damaged file (bad padding)")
	}
	p = p[:len(p)-n]
	if !bytes.HasPrefix(p, []byte("SBIN")) {
		return nil, errors.New("decrypted data is not SBIN")
	}
	return p, nil
}

func nfsEncrypt(name string, p []byte) []byte {
	n := 16 - len(p)%16
	buf := append(append([]byte(nil), p...), bytes.Repeat([]byte{byte(n)}, n)...)
	b, _ := aes.NewCipher(nfsKey)
	cipher.NewCBCEncrypter(b, nfsIV(name)).CryptBlocks(buf, buf)
	return buf
}

func fnv1(b []byte) uint32 {
	h := uint32(0x811c9dc5)
	for _, c := range b {
		h = h*0x01000193 ^ uint32(c)
	}
	return h
}

type sbChunk struct{ hdr, data, len int }

type SBIN struct {
	P      []byte
	Chunks map[string]sbChunk
	Strs   []string
	OHDR   []uint32
}

func parseSBIN(p []byte) (s *SBIN, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("bad SBIN (%v)", r)
		}
	}()
	s = &SBIN{P: p, Chunks: map[string]sbChunk{}}
	pos := 8
	for pos+12 <= len(p) {
		tag := string(p[pos : pos+4])
		ln := int(binary.LittleEndian.Uint32(p[pos+4:]))
		s.Chunks[tag] = sbChunk{pos, pos + 12, ln}
		pos = (pos + 12 + ln + 3) &^ 3
	}
	for _, t := range []string{"OHDR", "DATA", "CHDR", "CDAT"} {
		if _, ok := s.Chunks[t]; !ok {
			return nil, errors.New("missing chunk " + t)
		}
	}
	ch, cd := s.Chunks["CHDR"], s.Chunks["CDAT"]
	for i := 0; i+8 <= ch.len; i += 8 {
		a := int(binary.LittleEndian.Uint32(p[ch.data+i:]))
		l := int(binary.LittleEndian.Uint32(p[ch.data+i+4:]))
		s.Strs = append(s.Strs, string(p[cd.data+a:cd.data+a+l]))
	}
	oh := s.Chunks["OHDR"]
	for i := 0; i+4 <= oh.len; i += 4 {
		s.OHDR = append(s.OHDR, binary.LittleEndian.Uint32(p[oh.data+i:]))
	}
	return s, nil
}

func (s *SBIN) rehash(tag string) {
	c := s.Chunks[tag]
	binary.LittleEndian.PutUint32(s.P[c.hdr+8:], fnv1(s.P[c.data:c.data+c.len]))
}

// value types
const (
	sbInt32  = 0x05
	sbUint32 = 0x06
	sbInt64  = 0x08
	sbBool   = 0x09
	sbFloat  = 0x0a
	sbObj    = 0x0f
	sbStr    = 0x15
)

var sbSizes = map[int]int{0x01: 1, 0x02: 1, 0x03: 2, 0x04: 2, 0x05: 4, 0x06: 4, 0x07: 8, 0x08: 8, 0x09: 1, 0x0a: 4, 0x0b: 8, 0x0f: 4, 0x15: 2}

type NLeaf struct {
	File *NFSFile
	Path string
	Type int
	Off  int // absolute offset in plaintext
}

func (l *NLeaf) Editable() bool {
	switch l.Type {
	case sbInt32, sbUint32, sbInt64, sbBool, sbFloat:
		return true
	}
	return false
}

func (l *NLeaf) Get() string {
	p := l.File.S.P[l.Off:]
	switch l.Type {
	case sbInt32:
		return strconv.FormatInt(int64(int32(binary.LittleEndian.Uint32(p))), 10)
	case sbUint32:
		return strconv.FormatUint(uint64(binary.LittleEndian.Uint32(p)), 10)
	case sbInt64:
		return strconv.FormatInt(int64(binary.LittleEndian.Uint64(p)), 10)
	case sbBool:
		if p[0] != 0 {
			return "true"
		}
		return "false"
	case sbFloat:
		return strconv.FormatFloat(float64(math.Float32frombits(binary.LittleEndian.Uint32(p))), 'g', -1, 32)
	case sbStr:
		i := int(binary.LittleEndian.Uint16(p))
		if i < len(l.File.S.Strs) {
			return l.File.S.Strs[i]
		}
	case 0x01, 0x02:
		return strconv.Itoa(int(p[0]))
	case 0x03, 0x04:
		return strconv.Itoa(int(binary.LittleEndian.Uint16(p)))
	}
	return "?"
}

func (l *NLeaf) GetInt() int64 {
	v, _ := strconv.ParseInt(l.Get(), 10, 64)
	return v
}

func (l *NLeaf) Set(s string) error {
	s = strings.TrimSpace(s)
	p := l.File.S.P[l.Off:]
	clean := strings.NewReplacer(" ", "", ",", "", "_", "", " ", "").Replace(s)
	switch l.Type {
	case sbInt32:
		v, err := strconv.ParseInt(clean, 10, 32)
		if err != nil {
			return errors.New("enter a whole number (max 2147483647)")
		}
		binary.LittleEndian.PutUint32(p, uint32(int32(v)))
	case sbUint32:
		v, err := strconv.ParseUint(clean, 10, 32)
		if err != nil {
			return errors.New("enter a whole number")
		}
		binary.LittleEndian.PutUint32(p, uint32(v))
	case sbInt64:
		v, err := strconv.ParseInt(clean, 10, 64)
		if err != nil {
			return errors.New("enter a whole number")
		}
		binary.LittleEndian.PutUint64(p, uint64(v))
	case sbBool:
		switch strings.ToLower(s) {
		case "1", "true", "yes", "on":
			p[0] = 1
		case "0", "false", "no", "off":
			p[0] = 0
		default:
			return errors.New("enter true or false")
		}
	case sbFloat:
		f, err := strconv.ParseFloat(strings.Replace(s, ",", ".", 1), 32)
		if err != nil {
			return errors.New("enter a number")
		}
		binary.LittleEndian.PutUint32(p, math.Float32bits(float32(f)))
	default:
		return errors.New("this field cannot be edited")
	}
	l.File.Dirty = true
	return nil
}

type NFSFile struct {
	Name   string // e.g. 489867946_1.sb
	Path   string
	S      *SBIN
	OldMD5 string
	Dirty  bool
}

type NFSSave struct {
	Dir      string
	UID      string
	Files    []*NFSFile
	Manifest *NFSFile
	Leaves   []*NLeaf
	ByPath   map[string]*NLeaf
}

func (f *NFSFile) walk(add func(*NLeaf)) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("%s: unsupported structure (%v)", f.Name, r)
		}
	}()
	s := f.S
	d := s.Chunks["DATA"].data
	P := s.P
	u16 := func(o int) int { return int(binary.LittleEndian.Uint16(P[d+o:])) }
	u32 := func(o int) uint32 { return binary.LittleEndian.Uint32(P[d+o:]) }
	seen := map[uint32]bool{}
	var obj func(idx uint32, path string, depth int)
	// label for array elements: use _Id / key / SeriesId / Name string when present
	label := func(idx uint32) string {
		if idx == 0xffffffff || int(idx) >= len(s.OHDR) || s.OHDR[idx]&7 != 1 {
			return ""
		}
		off := int(s.OHDR[idx] >> 3)
		cnt := u16(off)
		q := off + 4
		for i := 0; i < cnt && i < 6; i++ {
			k, t, vo := u16(q), u16(q+2), int(u32(q+4))
			if t == sbStr {
				switch s.Strs[k] {
				case "_Id", "key", "SeriesId", "Name", "ChapterId", "_CardDescriptionId":
					return s.Strs[u16(off+vo)]
				}
			}
			sz, ok := sbSizes[t]
			if !ok {
				return ""
			}
			q = (off + vo + sz + 1) &^ 1
			for q+1 < len(P)-d && P[d+q] == 0xcd && P[d+q+1] == 0xcd {
				q += 2
			}
		}
		return ""
	}
	obj = func(idx uint32, path string, depth int) {
		if idx == 0xffffffff || depth > 64 || int(idx) >= len(s.OHDR) || seen[idx] {
			return
		}
		seen[idx] = true
		e := s.OHDR[idx]
		kind, off := e&7, int(e>>3)
		if kind == 2 {
			et, n := int(u32(off)), int(u32(off+4))
			sz, ok := sbSizes[et]
			if !ok {
				return
			}
			for i := 0; i < n; i++ {
				va := off + 8 + sz*i
				if et == sbObj {
					c := u32(va)
					l := label(c)
					if l == "" {
						l = strconv.Itoa(i)
					}
					obj(c, fmt.Sprintf("%s[%s]", path, l), depth+1)
				} else {
					add(&NLeaf{File: f, Path: fmt.Sprintf("%s[%d]", path, i), Type: et, Off: d + va})
				}
			}
			return
		}
		if kind != 1 {
			return
		}
		cnt := u16(off)
		q := off + 4
		for i := 0; i < cnt; i++ {
			k, t, vo := u16(q), u16(q+2), int(u32(q+4))
			va := off + vo
			name := path + "/" + s.Strs[k]
			if t == sbObj {
				obj(u32(va), name, depth+1)
			} else {
				add(&NLeaf{File: f, Path: name, Type: t, Off: d + va})
			}
			sz, ok := sbSizes[t]
			if !ok {
				return // unknown type: stop this object safely
			}
			q = (va + sz + 1) &^ 1
			for q+1 < len(P)-d && P[d+q] == 0xcd && P[d+q+1] == 0xcd {
				q += 2
			}
		}
	}
	if len(s.OHDR) > 0 {
		obj(0, "", 0)
	}
	return nil
}

var nfsSectionRe = regexp.MustCompile(`^(\d+)_(\d+)\.sb$`)

// LoadNFS loads every section of one player from a folder.
func LoadNFS(dir string) (*NFSSave, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	uid := ""
	for _, e := range ents {
		if strings.HasSuffix(e.Name(), "_m.sb") {
			uid = strings.TrimSuffix(e.Name(), "_m.sb")
			break
		}
	}
	if uid == "" {
		for _, e := range ents {
			if m := nfsSectionRe.FindStringSubmatch(e.Name()); m != nil {
				uid = m[1]
				break
			}
		}
	}
	if uid == "" {
		return nil, errors.New("no NFS No Limits save (*.sb) found in this folder")
	}
	sv := &NFSSave{Dir: dir, UID: uid, ByPath: map[string]*NLeaf{}}
	load := func(name string) (*NFSFile, error) {
		path := filepath.Join(dir, name)
		c, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		p, err := nfsDecrypt(name, c)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", name, err)
		}
		s, err := parseSBIN(p)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", name, err)
		}
		sum := md5.Sum(p)
		return &NFSFile{Name: name, Path: path, S: s, OldMD5: base64.StdEncoding.EncodeToString(sum[:])}, nil
	}
	var names []string
	for _, e := range ents {
		if m := nfsSectionRe.FindStringSubmatch(e.Name()); m != nil && m[1] == uid {
			names = append(names, e.Name())
		}
	}
	sort.Slice(names, func(i, j int) bool {
		a, _ := strconv.Atoi(nfsSectionRe.FindStringSubmatch(names[i])[2])
		b, _ := strconv.Atoi(nfsSectionRe.FindStringSubmatch(names[j])[2])
		return a < b
	})
	for _, n := range names {
		f, err := load(n)
		if err != nil {
			return nil, err
		}
		sv.Files = append(sv.Files, f)
		section := strings.TrimSuffix(n, ".sb")
		f.walk(func(l *NLeaf) {
			l.Path = sectionTitle(f, section) + l.Path
			sv.Leaves = append(sv.Leaves, l)
			if _, dup := sv.ByPath[l.Path]; !dup {
				sv.ByPath[l.Path] = l
			}
		})
	}
	if len(sv.Files) == 0 {
		return nil, errors.New("no section files found")
	}
	if _, err := os.Stat(filepath.Join(dir, uid+"_m.sb")); err == nil {
		m, err := load(uid + "_m.sb")
		if err != nil {
			return nil, err
		}
		sv.Manifest = m
	}
	return sv, nil
}

func sectionTitle(f *NFSFile, fallback string) string {
	for i, s := range f.S.Strs {
		if s == "SectionId" && i+1 < len(f.S.Strs) {
			return f.S.Strs[i+1]
		}
	}
	return fallback
}

// Save writes changed sections, updates the manifest hashes, keeps a backup.
func (sv *NFSSave) Save(stamp string) (string, error) {
	bdir := filepath.Join(sv.Dir, "save_editor_backup_"+stamp)
	var changed []*NFSFile
	for _, f := range sv.Files {
		if f.Dirty {
			changed = append(changed, f)
		}
	}
	if len(changed) == 0 {
		return "", errors.New("nothing changed")
	}
	if err := os.MkdirAll(bdir, 0755); err != nil {
		return "", err
	}
	backup := func(f *NFSFile) error {
		b, err := os.ReadFile(f.Path)
		if err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(bdir, f.Name), b, 0644)
	}
	type out struct {
		f   *NFSFile
		enc []byte
	}
	var outs []out
	manifestDirty := false
	for _, f := range changed {
		f.S.rehash("DATA")
		sum := md5.Sum(f.S.P)
		nm := base64.StdEncoding.EncodeToString(sum[:])
		if sv.Manifest != nil && nm != f.OldMD5 {
			if err := sv.Manifest.replaceString(f.OldMD5, nm); err != nil {
				return "", fmt.Errorf("manifest: %v", err)
			}
			manifestDirty = true
		}
		f.OldMD5 = nm
		outs = append(outs, out{f, nfsEncrypt(f.Name, f.S.P)})
	}
	if manifestDirty {
		sv.Manifest.S.rehash("CDAT")
		outs = append(outs, out{sv.Manifest, nfsEncrypt(sv.Manifest.Name, sv.Manifest.S.P)})
	}
	// verify before touching disk
	for _, o := range outs {
		if p, err := nfsDecrypt(o.f.Name, o.enc); err != nil || !bytes.Equal(p, o.f.S.P) {
			return "", errors.New("self-check failed, nothing written")
		}
	}
	for _, o := range outs {
		if err := backup(o.f); err != nil {
			return "", err
		}
	}
	for _, o := range outs {
		if err := os.WriteFile(o.f.Path, o.enc, 0644); err != nil {
			return "", err
		}
		o.f.Dirty = false
	}
	return bdir, nil
}

// replaceString swaps a same-length string in CDAT.
func (f *NFSFile) replaceString(old, nw string) error {
	if len(old) != len(nw) {
		return errors.New("hash length mismatch")
	}
	s := f.S
	ch, cd := s.Chunks["CHDR"], s.Chunks["CDAT"]
	found := false
	for i := 0; i+8 <= ch.len; i += 8 {
		a := int(binary.LittleEndian.Uint32(s.P[ch.data+i:]))
		l := int(binary.LittleEndian.Uint32(s.P[ch.data+i+4:]))
		if l == len(old) && string(s.P[cd.data+a:cd.data+a+l]) == old {
			copy(s.P[cd.data+a:], nw)
			s.Strs[i/8] = nw
			found = true
		}
	}
	if !found {
		return errors.New("section hash not found (manifest out of date?)")
	}
	return nil
}

// ---- friendly fields ----

type NFSMain struct {
	Label string
	Path  string
	Also  string // ledger field that gets the same delta (keeps totals consistent)
}

func nfsCurrency(id, label string) NFSMain {
	base := "Currencies/ProgressionModel/_CurrencyInventory/_Currencies[" + id + "]"
	return NFSMain{label, base + "/_BalanceEncrypted/Value", base + "/_BalanceEarned/Value"}
}

var nfsMain = []NFSMain{
	nfsCurrency("Cash", "Cash"),
	nfsCurrency("PC", "Gold"),
	{"Driver level", "Unspecified/ProgressionModel/_SPLevelEncrypted/Value", ""},
	{"Rep (XP)", "Unspecified/ProgressionModel/_RepEncrypted/Value", ""},
	{"Mechanic points", "Unspecified/ProgressionModel/_MechanicPoints/Value", ""},
	nfsCurrency("Fuel", "Fuel"),
	nfsCurrency("Scrap", "Scrap"),
	nfsCurrency("RaceSkips", "Race skips"),
	nfsCurrency("TuningTools", "Tuning tools"),
	nfsCurrency("TunerTrialKeys", "Tuner trial keys"),
	nfsCurrency("VP", "VP"),
	nfsCurrency("TournamentCurrency", "Tournament currency"),
	nfsCurrency("VIPP", "VIP points"),
	nfsCurrency("LTSGrind", "LTS currency"),
}

func (sv *NFSSave) SetMain(m NFSMain, val string) error {
	l := sv.ByPath[m.Path]
	if l == nil {
		return errors.New(m.Label + " not in this save")
	}
	old := l.GetInt()
	if err := l.Set(val); err != nil {
		return fmt.Errorf("%s: %v", m.Label, err)
	}
	nv := l.GetInt()
	if a := sv.ByPath[m.Also]; a != nil && nv > old {
		e := a.GetInt() + (nv - old)
		if e > math.MaxInt32 {
			e = math.MaxInt32
		}
		a.Set(strconv.FormatInt(e, 10))
	}
	return nil
}
