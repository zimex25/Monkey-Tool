package main

// Real Racing 3 save format (character.2.dat and friends)
//
//  file   = plaintext XOR key64 (repeating 64-byte key)
//  plain  = header | dictionary | root object | object table | checksum
//  header = magic ABCFFCBA, version, total size, name count, key count (all u32 BE)
//  checksum (last byte) = XOR of every plaintext byte before it

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

var rr3Key = []byte{
	0x64, 0x95, 0xe4, 0x50, 0xcf, 0xd2, 0x0c, 0x32, 0x54, 0x82, 0xfc, 0x43, 0x80, 0x13, 0xa1, 0x5e,
	0x0b, 0x46, 0x5c, 0xd7, 0xc6, 0x87, 0xa8, 0xa9, 0x55, 0xa7, 0x94, 0x10, 0x11, 0x33, 0xe7, 0x67,
	0xb0, 0xc9, 0xef, 0xda, 0x21, 0x11, 0x3d, 0x91, 0x8d, 0x88, 0x4c, 0xc9, 0xfa, 0x81, 0x5d, 0x9f,
	0x17, 0x1c, 0xda, 0x97, 0x4b, 0x9c, 0xac, 0x92, 0x60, 0x7f, 0xff, 0x3c, 0x40, 0xc6, 0x50, 0x00,
}

const rr3Magic = 0xABCFFCBA

func rr3Crypt(b []byte) []byte {
	out := make([]byte, len(b))
	for i, c := range b {
		out[i] = c ^ rr3Key[i%64]
	}
	return out
}

// Value tags (low 3 bits = type)
const (
	tBool   = 0 // payload in bit 3
	tUint   = 1 // inline (bits 3-6) or 0x81 + varint
	tFloat  = 2 // 0x02 + float32 LE
	tString = 3 // 0x03 + zero-terminated string
	tRef    = 4 // 0x04 + varint object index
	tBlob   = 5 // 0x05 + u32 BE length + bytes
)

type Rec struct {
	Key      uint32
	Name     string
	KeyBytes []byte
	Tag      byte
	Raw      []byte // encoded value incl. tag
	U        uint64
	F        float32
	S        string
	Blob     []byte
}

func (r *Rec) Type() int { return int(r.Tag & 7) }

func (r *Rec) ValueString() string {
	switch r.Type() {
	case tBool:
		if r.U != 0 {
			return "true"
		}
		return "false"
	case tUint:
		return strconv.FormatUint(r.U, 10)
	case tFloat:
		return strconv.FormatFloat(float64(r.F), 'g', -1, 32)
	case tString:
		return r.S
	case tRef:
		return fmt.Sprintf("<object #%d>", r.U)
	case tBlob:
		if len(r.Blob) == 4 {
			return strconv.FormatUint(uint64(binary.LittleEndian.Uint32(r.Blob)), 10)
		}
		if len(r.Blob) == 8 {
			return "<hidden 64-bit>"
		}
		return fmt.Sprintf("<%d bytes>", len(r.Blob))
	}
	return "?"
}

func (r *Rec) Editable() bool {
	switch r.Type() {
	case tBool, tUint, tFloat, tString:
		return true
	case tBlob:
		return len(r.Blob) == 4
	}
	return false
}

type RR3Save struct {
	Path     string
	Plain    []byte
	Names    map[uint32]string
	Root     []*Rec
	rootFrom int
	rootTo   int
	ByName   map[string]*Rec
}

func uvarint(d []byte, p int) (uint64, int, error) {
	var v uint64
	var s uint
	for {
		if p >= len(d) {
			return 0, p, errors.New("unexpected end of data")
		}
		b := d[p]
		p++
		v |= uint64(b&0x7f) << s
		s += 7
		if b < 0x80 {
			return v, p, nil
		}
		if s > 63 {
			return 0, p, errors.New("bad varint")
		}
	}
}

func putUvarint(v uint64) []byte {
	var out []byte
	for {
		b := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			out = append(out, b|0x80)
		} else {
			out = append(out, b)
			return out
		}
	}
}

func LoadRR3(path string) (*RR3Save, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return ParseRR3(path, raw)
}

func ParseRR3(path string, raw []byte) (sv *RR3Save, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("corrupt save (%v)", r)
		}
	}()
	d := rr3Crypt(raw)
	if len(d) < 25 || binary.BigEndian.Uint32(d) != rr3Magic {
		return nil, errors.New("not a Real Racing 3 save (bad header after decryption)")
	}
	if int(binary.BigEndian.Uint32(d[8:])) != len(d) {
		return nil, errors.New("size field does not match file size")
	}
	var x byte
	for _, c := range d[:len(d)-1] {
		x ^= c
	}
	if x != d[len(d)-1] {
		return nil, errors.New("checksum mismatch - file is damaged")
	}
	nNames := int(binary.BigEndian.Uint32(d[12:]))
	sv = &RR3Save{Path: path, Plain: d, Names: map[uint32]string{}, ByName: map[string]*Rec{}}
	p := 20
	for i := 0; i < nNames; i++ {
		e := p
		for d[e] >= 0x20 {
			e++
		}
		name := string(d[p:e])
		t := int(d[e])
		p = e + 1
		if t == 0 {
			var v uint64
			v, p, err = uvarint(d, p)
			if err != nil {
				return nil, err
			}
			sv.Names[uint32(v)] = name
			continue
		}
		var n uint64
		n, p, err = uvarint(d, p)
		if err != nil {
			return nil, err
		}
		for j := uint64(0); j < n; j++ {
			nm := name
			for k := 0; k < t; k++ {
				var id uint64
				id, p, err = uvarint(d, p)
				if err != nil {
					return nil, err
				}
				nm = strings.Replace(nm, "[id]", "["+strconv.FormatUint(id, 10)+"]", 1)
			}
			var v uint64
			v, p, err = uvarint(d, p)
			if err != nil {
				return nil, err
			}
			sv.Names[uint32(v)] = nm
		}
	}
	// root object
	cnt := int(binary.BigEndian.Uint32(d[p:]))
	p += 4
	sv.rootFrom = p
	for i := 0; i < cnt; i++ {
		r := &Rec{}
		ks := p
		k := uint32(d[p])<<8 | uint32(d[p+1])
		if k&0x8000 != 0 {
			k = (k & 0x7fff) | (uint32(d[p+2])<<8|uint32(d[p+3]))<<15
			p += 4
		} else {
			p += 2
		}
		r.Key = k
		r.KeyBytes = d[ks:p]
		r.Name = sv.Names[k]
		vs := p
		t := d[p]
		r.Tag = t
		p++
		switch {
		case t&0x80 != 0:
			if t&7 != tUint {
				return nil, fmt.Errorf("unknown value tag %02x at %x", t, vs)
			}
			r.U, p, err = uvarint(d, p)
			if err != nil {
				return nil, err
			}
		case t&7 == tBool || t&7 == tUint:
			r.U = uint64(t >> 3)
		case t == tFloat:
			r.F = math.Float32frombits(binary.LittleEndian.Uint32(d[p:]))
			p += 4
		case t == tString:
			e := p
			for d[e] != 0 {
				e++
			}
			r.S = string(d[p:e])
			p = e + 1
		case t == tRef:
			r.U, p, err = uvarint(d, p)
			if err != nil {
				return nil, err
			}
		case t == tBlob:
			l := int(binary.BigEndian.Uint32(d[p:]))
			p += 4
			r.Blob = append([]byte(nil), d[p:p+l]...)
			p += l
		default:
			return nil, fmt.Errorf("unknown value tag %02x at %x", t, vs)
		}
		r.Raw = append([]byte(nil), d[vs:p]...)
		sv.Root = append(sv.Root, r)
		if _, dup := sv.ByName[r.Name]; !dup {
			sv.ByName[r.Name] = r
		}
	}
	sv.rootTo = p
	return sv, nil
}

// ---- setters ----

func encUint(origTag byte, v uint64) []byte {
	if v < 16 && origTag != 0x81 {
		return []byte{byte(v<<3) | tUint}
	}
	return append([]byte{0x81}, putUvarint(v&0xffffffff)...)
}

func (r *Rec) SetUint(v uint64) {
	r.U = v & 0xffffffff
	r.Raw = encUint(r.Tag, r.U)
	r.Tag = r.Raw[0]
}

func (r *Rec) SetBlob(b []byte) {
	r.Blob = append([]byte(nil), b...)
	raw := []byte{tBlob, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(raw[1:], uint32(len(b)))
	r.Raw = append(raw, b...)
}

// SetFromString parses user text for a plain (editable) field.
func (r *Rec) SetFromString(s string) error {
	s = strings.TrimSpace(s)
	switch r.Type() {
	case tBool:
		var b bool
		switch strings.ToLower(s) {
		case "1", "true", "yes", "on":
			b = true
		case "0", "false", "no", "off":
		default:
			return errors.New("enter true or false")
		}
		if b {
			r.U, r.Raw = 1, []byte{0x08}
		} else {
			r.U, r.Raw = 0, []byte{0x00}
		}
		r.Tag = r.Raw[0]
	case tUint:
		v, err := parseNum(s)
		if err != nil {
			return err
		}
		r.SetUint(v)
	case tFloat:
		f, err := strconv.ParseFloat(strings.Replace(s, ",", ".", 1), 32)
		if err != nil {
			return errors.New("enter a number")
		}
		r.F = float32(f)
		r.Raw = make([]byte, 5)
		r.Raw[0] = tFloat
		binary.LittleEndian.PutUint32(r.Raw[1:], math.Float32bits(r.F))
	case tString:
		if strings.ContainsRune(s, 0) {
			return errors.New("invalid text")
		}
		r.S = s
		r.Raw = append(append([]byte{tString}, s...), 0)
	case tBlob:
		if len(r.Blob) != 4 {
			return errors.New("this field cannot be edited")
		}
		v, err := parseNum(s)
		if err != nil {
			return err
		}
		b := make([]byte, 4)
		binary.LittleEndian.PutUint32(b, uint32(v))
		r.SetBlob(b)
	default:
		return errors.New("this field cannot be edited")
	}
	return nil
}

func parseNum(s string) (uint64, error) {
	s = strings.NewReplacer(" ", "", ",", "", ".", "", "_", "", " ", "").Replace(s)
	if strings.HasPrefix(s, "-") {
		v, err := strconv.ParseInt(s, 10, 32)
		if err != nil {
			return 0, errors.New("enter a whole number")
		}
		return uint64(uint32(int32(v))), nil
	}
	v, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return 0, errors.New("enter a whole number between 0 and 4294967295")
	}
	return v, nil
}

// Build re-serialises the save (plaintext -> encrypted bytes).
func (sv *RR3Save) Build() []byte {
	d := sv.Plain
	var out []byte
	out = append(out, d[:sv.rootFrom]...)
	for _, r := range sv.Root {
		out = append(out, r.KeyBytes...)
		out = append(out, r.Raw...)
	}
	out = append(out, d[sv.rootTo:len(d)-1]...)
	out = append(out, 0)
	binary.BigEndian.PutUint32(out[8:], uint32(len(out)))
	var x byte
	for _, c := range out[:len(out)-1] {
		x ^= c
	}
	out[len(out)-1] = x
	return rr3Crypt(out)
}

// Save writes the file, keeping a timestamped backup, and refreshes the copy inside TempSaveGame.dat.
func (sv *RR3Save) Save(stamp string) (string, error) {
	enc := sv.Build()
	if _, err := ParseRR3(sv.Path, enc); err != nil {
		return "", fmt.Errorf("self-check failed, nothing written: %v", err)
	}
	orig, err := os.ReadFile(sv.Path)
	if err != nil {
		return "", err
	}
	bdir := filepath.Join(filepath.Dir(sv.Path), "save_editor_backup_"+stamp)
	if err := os.MkdirAll(bdir, 0755); err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(bdir, filepath.Base(sv.Path)), orig, 0644); err != nil {
		return "", err
	}
	tmp := filepath.Join(filepath.Dir(sv.Path), "TempSaveGame.dat")
	if tb, err := os.ReadFile(tmp); err == nil {
		os.WriteFile(filepath.Join(bdir, "TempSaveGame.dat"), tb, 0644)
		if nb, ok := replaceInTempSave(tb, filepath.Base(sv.Path), enc); ok {
			if err := os.WriteFile(tmp, nb, 0644); err != nil {
				return "", err
			}
		}
	}
	if err := os.WriteFile(sv.Path, enc, 0644); err != nil {
		return "", err
	}
	s2, err := LoadRR3(sv.Path)
	if err != nil {
		return "", fmt.Errorf("written file failed verification: %v (backup in %s)", err, bdir)
	}
	*sv = *s2
	return bdir, nil
}

// TempSaveGame.dat: u32 version, u32 count, then {u32 nameLen, name, u32 size, data} (LE)
func replaceInTempSave(tb []byte, name string, data []byte) ([]byte, bool) {
	if len(tb) < 8 {
		return nil, false
	}
	cnt := int(binary.LittleEndian.Uint32(tb[4:]))
	out := append([]byte(nil), tb[:8]...)
	p := 8
	found := false
	for i := 0; i < cnt; i++ {
		if p+4 > len(tb) {
			return nil, false
		}
		nl := int(binary.LittleEndian.Uint32(tb[p:]))
		if p+4+nl+4 > len(tb) {
			return nil, false
		}
		nm := string(tb[p+4 : p+4+nl])
		sz := int(binary.LittleEndian.Uint32(tb[p+4+nl:]))
		ds := p + 8 + nl
		if ds+sz > len(tb) {
			return nil, false
		}
		out = append(out, tb[p:p+4+nl]...)
		payload := tb[ds : ds+sz]
		if nm == name {
			payload = data
			found = true
		}
		l := make([]byte, 4)
		binary.LittleEndian.PutUint32(l, uint32(len(payload)))
		out = append(out, l...)
		out = append(out, payload...)
		p = ds + sz
	}
	return out, found && p == len(tb)
}

// ---- hidden (EHV) values ----

type Hidden struct {
	Label   string
	Field   string
	KeyName string // shared key id in keys.ini
	Wide    bool   // 64-bit blob
}

var rr3Hidden = []Hidden{
	{"R$", "m_CurrencyWallet.m_RDollars.m_Balance[EHV].nValue", "rdollars", true},
	{"Gold", "m_CurrencyWallet.m_Gold.m_Balance[EHV].nValue", "gold", true},
	{"M$", "m_CurrencyWallet.m_MDollars.m_Balance[EHV].nValue", "mdollars", true},
	{"Driver level", "m_xp.m_currentDriverLevel[EHV].nValue", "level", false},
}

func (sv *RR3Save) hiddenStored(h Hidden) (uint64, bool) {
	r := sv.ByName[h.Field]
	if r == nil {
		return 0, false
	}
	if h.Wide {
		if len(r.Blob) != 8 {
			return 0, false
		}
		return binary.LittleEndian.Uint64(r.Blob), true
	}
	if r.Type() != tUint {
		return 0, false
	}
	return r.U, true
}

func (sv *RR3Save) HiddenGet(h Hidden, keys map[string]uint64) (uint64, bool) {
	st, ok := sv.hiddenStored(h)
	k, kok := keys[h.KeyName]
	if !ok || !kok {
		return 0, false
	}
	return st ^ k, true
}

func (sv *RR3Save) HiddenSet(h Hidden, keys map[string]uint64, v uint64) error {
	r := sv.ByName[h.Field]
	k, kok := keys[h.KeyName]
	if r == nil || !kok {
		return errors.New("not calibrated")
	}
	if h.Wide {
		b := make([]byte, 8)
		binary.LittleEndian.PutUint64(b, v^k)
		r.SetBlob(b)
	} else {
		r.U = (v ^ k) & 0xffffffff
		r.Raw = append([]byte{0x81}, putUvarint(r.U)...)
		r.Tag = 0x81
	}
	return nil
}

// Calibrate derives the key from the value the game currently shows.
func (sv *RR3Save) Calibrate(h Hidden, keys map[string]uint64, shown uint64) error {
	st, ok := sv.hiddenStored(h)
	if !ok {
		return errors.New(h.Label + " field not found in this save")
	}
	keys[h.KeyName] = st ^ shown
	return nil
}

func (sv *RR3Save) SortedRoot() []*Rec {
	out := append([]*Rec(nil), sv.Root...)
	sort.SliceStable(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

// Built-in RR3 hidden-value keys. Empty until confirmed from a real save;
// the Calibrate button fills them in per user (stored in %APPDATA%).
var rr3DefaultKeys = map[string]uint64{}
