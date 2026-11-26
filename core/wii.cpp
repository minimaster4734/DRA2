#include "wii.h"
#include "jit_rt.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>
s32 btIoctlv(Wii& w, u32 a, bool& defer);
void btTick(Wii& w);
u64 btNextDue(Wii& w);
std::string btDebug(Wii& w);

// ------------------------------------------------------------ crypto (AES-128 decrypt, SHA-1)
namespace {
u8 SB[256], ISB[256];
void aesInit() {
  static bool done = false; if (done) return; done = true;
  u8 p = 1, q = 1;
  do { p = p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0); q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
       u8 x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4); SB[p] = x ^ 0x63; } while (p != 1);
  SB[0] = 0x63; for (int i = 0; i < 256; i++) ISB[SB[i]] = i;
}
inline u8 xt(u8 x) { return (x << 1) ^ ((x & 0x80) ? 0x1B : 0); }
inline u8 mul(u8 x, u8 y) { u8 r = 0; while (y) { if (y & 1) r ^= x; x = xt(x); y >>= 1; } return r; }
struct Aes {
  u8 rk[176];
  void setKey(const u8* k) {
    aesInit(); memcpy(rk, k, 16); u8 rc = 1;
    for (int i = 16; i < 176; i += 4) {
      u8 t[4]; memcpy(t, rk + i - 4, 4);
      if (i % 16 == 0) { u8 a = t[0]; t[0] = SB[t[1]] ^ rc; t[1] = SB[t[2]]; t[2] = SB[t[3]]; t[3] = SB[a]; rc = xt(rc); }
      for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
  }
  void dec(const u8* in, u8* out) {
    u8 s[16]; for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
    for (int r = 9; r >= 0; r--) {
      u8 t[16];   // inverse shift rows + inverse sub bytes
      for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[((c + row) & 3) * 4 + row] = ISB[s[c * 4 + row]];
      for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[r * 16 + i];
      if (r) for (int c = 0; c < 4; c++) { u8* x = s + c * 4; u8 a = x[0], b = x[1], cc = x[2], d = x[3];
        x[0] = mul(a, 14) ^ mul(b, 11) ^ mul(cc, 13) ^ mul(d, 9); x[1] = mul(a, 9) ^ mul(b, 14) ^ mul(cc, 11) ^ mul(d, 13);
        x[2] = mul(a, 13) ^ mul(b, 9) ^ mul(cc, 14) ^ mul(d, 11); x[3] = mul(a, 11) ^ mul(b, 13) ^ mul(cc, 9) ^ mul(d, 14); }
    }
    memcpy(out, s, 16);
  }
  void cbc(const u8 iv[16], const u8* in, u8* out, size_t n) {
    u8 prev[16], cur[16]; memcpy(prev, iv, 16);
    for (size_t i = 0; i < n; i += 16) { memcpy(cur, in + i, 16); dec(cur, out + i); for (int j = 0; j < 16; j++) out[i + j] ^= prev[j]; memcpy(prev, cur, 16); }
  }
};
void sha1(const u8* d, size_t n, u8 out[20]) {
  u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  auto rol = [](u32 x, int s) { return x << s | x >> (32 - s); };
  std::vector<u8> t(d, d + n); t.push_back(0x80); while (t.size() % 64 != 56) t.push_back(0);
  u64 bits = (u64)n * 8; for (int i = 7; i >= 0; i--) t.push_back((u8)(bits >> (8 * i)));
  for (size_t o = 0; o < t.size(); o += 64) {
    u32 w[80]; for (int i = 0; i < 16; i++) w[i] = t[o + i * 4] << 24 | t[o + i * 4 + 1] << 16 | t[o + i * 4 + 2] << 8 | t[o + i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4];
    for (int i = 0; i < 80; i++) { u32 f, k;
      if (i < 20) { f = (b & c) | (~b & dd); k = 0x5A827999; } else if (i < 40) { f = b ^ c ^ dd; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & dd) | (c & dd); k = 0x8F1BBCDC; } else { f = b ^ c ^ dd; k = 0xCA62C1D6; }
      u32 tmp = rol(a, 5) + f + e + k + w[i]; e = dd; dd = c; c = rol(b, 30); b = a; a = tmp; }
    h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e;
  }
  for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = h[i] >> (24 - 8 * j);
}
bool lz11(const u8* d, size_t n, std::vector<u8>& o) {
  if (n < 8 || d[0] != 0x11) return false;
  size_t sz = d[1] | d[2] << 8 | d[3] << 16, p = 4; if (!sz) { sz = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24; p = 8; }
  if (sz > (64u << 20)) return false;
  o.clear(); o.reserve(sz);
  while (o.size() < sz && p < n) {
    u8 fl = d[p++];
    for (int i = 0; i < 8 && o.size() < sz; i++) {
      if (fl & (0x80 >> i)) {
        if (p + 1 >= n) return false; u8 b0 = d[p]; u32 ln, disp;
        if ((b0 >> 4) == 0) { if (p + 2 >= n) return false; ln = ((b0 & 15) << 4 | d[p + 1] >> 4) + 0x11; disp = ((d[p + 1] & 15) << 8 | d[p + 2]) + 1; p += 3; }
        else if ((b0 >> 4) == 1) { if (p + 3 >= n) return false; ln = ((b0 & 15) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111; disp = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1; p += 4; }
        else { ln = (b0 >> 4) + 1; disp = ((b0 & 15) << 8 | d[p + 1]) + 1; p += 2; }
        if (disp > o.size()) return false;
        for (u32 k = 0; k < ln && o.size() < sz; k++) o.push_back(o[o.size() - disp]);
      } else { if (p >= n) return false; o.push_back(d[p++]); }
    }
  }
  return o.size() == sz;
}
inline u32 be32(const u8* p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline u64 be64(const u8* p) { return (u64)be32(p) << 32 | be32(p + 4); }
inline u32 al64(u32 x) { return (x + 63) & ~63u; }
}

// ------------------------------------------------------------ WAD
bool Wii::open(const char* file, const u8 key[16]) {
  wadPath = file; wad = fopen(file, "rb"); if (!wad) { err = "cannot open WAD"; return false; }
  u8 h[0x40]; if (fread(h, 1, 0x40, wad) != 0x40 || be32(h) != 0x20 || (h[4] != 'I' && h[4] != 'i') || h[5] != 's') { err = "not a WAD file (Is/ib header missing)"; return false; }
  u32 cc = be32(h + 8), tik = be32(h + 0x10), tm = be32(h + 0x14);
  u32 tikOff = al64(0x20) + al64(cc), tmdOff = tikOff + al64(tik), dataOff = tmdOff + al64(tm);
  std::vector<u8> tk(tik); tmd.resize(tm);
  fseek(wad, tikOff, SEEK_SET); if (fread(tk.data(), 1, tik, wad) != tik) { err = "truncated ticket"; return false; }
  fseek(wad, tmdOff, SEEK_SET); if (fread(tmd.data(), 1, tm, wad) != tm) { err = "truncated TMD"; return false; }
  if (tik < 0x2A4 || tm < 0x1E4) { err = "bad ticket/TMD size"; return false; }
  titleId = be64(tk.data() + 0x1DC);
  Aes a; a.setKey(key); u8 iv[16] = {}; memcpy(iv, tk.data() + 0x1DC, 8); a.cbc(iv, tk.data() + 0x1BF, titleKey, 16);
  iosNum = be32(tmd.data() + 0x188);
  u32 n = tmd[0x1DE] << 8 | tmd[0x1DF]; u64 off = dataOff;
  for (u32 i = 0; i < n; i++) {
    const u8* c = tmd.data() + 0x1E4 + 36 * i; WiiContent w{}; w.cid = be32(c); w.index = c[4] << 8 | c[5]; w.type = c[6] << 8 | c[7]; w.size = be64(c + 8); memcpy(w.sha, c + 16, 20);
    w.fileOff = off; off += al64((u32)((w.size + 15) & ~15ull)); contents.push_back(w);
  }
  return true;
}
bool Wii::readContent(int index, std::vector<u8>& out) {
  for (auto& c : contents) if (c.index == index) {
    size_t n = (c.size + 15) & ~15ull; std::vector<u8> enc(n); out.assign(n, 0);
    fseeko(wad, c.fileOff, SEEK_SET); if (fread(enc.data(), 1, n, wad) != n) { err = "truncated WAD"; return false; }
    Aes a; a.setKey(titleKey); u8 iv[16] = {}; iv[0] = c.index >> 8; iv[1] = c.index; a.cbc(iv, enc.data(), out.data(), n);
    out.resize(c.size); u8 h[20]; sha1(out.data(), out.size(), h);
    if (memcmp(h, c.sha, 20)) { err = "content hash mismatch: wrong common key, or damaged WAD"; return false; }
    return true; }
  err = "no such content"; return false;
}
bool Wii::findGameDol(std::vector<u8>& dol) {
  for (auto& c : contents) {
    if (c.size < 0x1000 || (c.type & 0x8000)) continue;
    u8 enc[16], pt[16]; fseeko(wad, c.fileOff, SEEK_SET); if (fread(enc, 1, 16, wad) != 16) continue;
    Aes a; a.setKey(titleKey); u8 iv[16] = {}; iv[0] = c.index >> 8; iv[1] = c.index; a.cbc(iv, enc, pt, 16);
    if (pt[0] != 0x11) continue;
    std::vector<u8> raw, dec; if (!readContent(c.index, raw)) return false;
    if (!lz11(raw.data(), raw.size(), dec) || dec.size() < 0x100 || be32(dec.data()) != 0x100 || be32(dec.data() + 0xE0) < 0x80000000u) continue;
    dol.swap(dec); return true;
  }
  err = "no packed game executable found in this WAD"; return false;
}

// ------------------------------------------------------------ boot
// Minimal SYSCONF (system settings) so the SDK's Bluetooth stack considers the virtual Wii Remote paired.
// Layout: "SCv0", item count, offsets (+ end offset), items {type<<5|namelen-1, name, u16 len-1, data}, footer "SCed".
static void ensureSysconf(const std::string& nand) {
  std::string dir = nand + "/shared2/sys", path = dir + "/SYSCONF";
  mkdir((nand + "/shared2").c_str(), 0755); mkdir(dir.c_str(), 0755);
  if (FILE* f = fopen(path.c_str(), "rb")) { std::vector<u8> d(16384); size_t n = fread(d.data(), 1, d.size(), f); fclose(f);
    static const u8 nm[] = "Nintendo RVL-CNT-01"; if (n == 16384 && std::search(d.begin(), d.end(), nm, nm + sizeof(nm) - 1) != d.end()) return; }
  static const u8 addr[6] = {0x00, 0x19, 0x1D, 0xAA, 0xBB, 0xCC};
  std::vector<u8> dinf(0x461, 0), cdif(0x205, 0);
  dinf[0] = 1; memcpy(&dinf[1], addr, 6); memcpy(&dinf[7], "Nintendo RVL-CNT-01", 19);
  memcpy(&dinf[1 + 10 * 70], addr, 6); memcpy(&dinf[7 + 10 * 70], "Nintendo RVL-CNT-01", 19);   // registered[0] and active[0]
  cdif[0] = 1; memcpy(&cdif[4], addr, 6); for (int i = 0; i < 16; i++) cdif[10 + i] = (u8)(0x31 + i * 7);
  std::vector<u8> out(16384, 0);
  auto item = [&](const char* name, const std::vector<u8>& data, size_t at) { size_t nl = strlen(name); out[at] = (u8)((1 << 5) | (nl - 1)); memcpy(&out[at + 1], name, nl);
    out[at + 1 + nl] = (u8)((data.size() - 1) >> 8); out[at + 2 + nl] = (u8)((data.size() - 1)); memcpy(&out[at + 3 + nl], data.data(), data.size()); return at + 3 + nl + data.size(); };
  memcpy(&out[0], "SCv0", 4); out[5] = 2;
  size_t o1 = 0x0c, o2 = item("BT.DINF", dinf, o1), o3 = item("BT.CDIF", cdif, o2);
  out[6] = o1 >> 8; out[7] = o1; out[8] = o2 >> 8; out[9] = o2; out[10] = o3 >> 8; out[11] = o3;
  memcpy(&out[16380], "SCed", 4);
  if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
}

bool Wii::boot(Machine* mach) {
  m = mach; m->wii = this; m->bus.wii = true; trace = getenv("WIITRACE") != nullptr;
  std::vector<u8> d; if (!findGameDol(d)) return false;
  Bus& bus = m->bus; u32 bss = be32(&d[0xD8]), bssSz = be32(&d[0xDC]), entry = be32(&d[0xE0]);
  if (bss && bssSz) memset(bus.ram + (bss & 0x1FFFFFFF), 0, bssSz);
  for (int i = 0; i < 18; i++) {
    u32 off = be32(&d[i * 4]), addr = be32(&d[0x48 + i * 4]), sz = be32(&d[0x90 + i * 4]); if (!sz) continue;
    if (off + (u64)sz > d.size() || !inRamPhys(addr & 0x1FFFFFFF, sz)) { err = "DOL section out of range"; return false; }
    memcpy(bus.ram + (addr & 0x1FFFFFFF), d.data() + off, sz);
  }
  u8* L = bus.ram; auto w32 = [&](u32 o, u32 v) { *(u32*)(L + o) = bswap32(v); };
  u32 tl = (u32)titleId; w32(0, tl); w32(4, 0x30380000u);   // game id + maker "08"
  w32(0x20, 0x0D15EA5E); w32(0x24, 1); w32(0x28, 0x01800000); w32(0x2C, 0x00000023);
  w32(0x30, 0); w32(0x34, 0x817FEC60u); w32(0xCC, 1); w32(0xE4, 0x8008F7B8u); w32(0xF0, 0x01800000); w32(0xF8, 243000000); w32(0xFC, 729000000);
  w32(0x3100, 0x01800000); w32(0x3104, 0x01800000); w32(0x3108, 0x81800000u); w32(0x310C, 0); w32(0x3110, 0x81700000u);
  w32(0x3118, 0x04000000); w32(0x311C, 0x04000000); w32(0x3120, 0x93400000u); w32(0x3124, 0x90002000u); w32(0x3128, 0x933E0000u);
  w32(0x3130, 0x933E0000u); w32(0x3134, 0x93400000u); w32(0x3138, 0x11);
  w32(0x3140, (iosNum << 16) | 0x161F); w32(0x3144, 0x00062507); w32(0x3180, tl); w32(0x3188, (iosNum << 16) | 0x161F);   // minimum IOS the title was built against (the SDK compares it with 0x3140)
  m->cpu.bus = &bus; m->cpu.m = m; bus.m = m; m->cpu.pc = entry; m->cpu.msr = 0x2000; m->cpu.gpr[1] = 0x816FFFF0;
  hw[0x214 / 4] = 0x11;
  if (const char* d2 = getenv("WII_NAND")) nandDir = d2; else nandDir = "wii_nand";
  mkdir(nandDir.c_str(), 0755); ensureSysconf(nandDir);
  m->log("wii boot: title %016llX ios%u entry=%08X (%zu bytes DOL)", (unsigned long long)titleId, iosNum, entry, d.size());
  return true;
}

// ------------------------------------------------------------ Hollywood registers + IPC
bool Wii::irq() const { return ((ppcCtrl & 4) && (ppcCtrl & 0x10)) || ((ppcCtrl & 2) && (ppcCtrl & 0x20)); }
u32 Bus::hollyRead(u32 off, int size) { if (off >= 0x1000 && off < 0x7000) return hwRead(off, size); return m->wii->ipcRead(off) & (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFF : 0xFF); }
void Bus::hollyWrite(u32 off, u32 val, int size) { if (off >= 0x1000 && off < 0x7000) { hwWrite(off, val, size); return; } m->wii->ipcWrite(off, val); }
u32 Wii::ipcRead(u32 off) {
  switch (off) { case 0: return ppcMsg; case 4: return ppcCtrl; case 8: return armMsg; case 0xC: return armCtrl; }
  if (off >= 0x800000 && off < 0x802000) return hw[(off - 0x800000) >> 2];
  return 0;
}
void Wii::ipcWrite(u32 off, u32 v) {
  if (off == 0) { ppcMsg = v; return; }
  if (off == 4) {
    ppcCtrl = (ppcCtrl & ~0x30u) | (v & 0x30); ppcCtrl &= ~(v & 6);
    if (v & 1) { ppcCtrl |= 2; u32 a = ppcMsg; handle(a); }   // IOS takes the request and acks it
    m->updateInts(); updateDue(); return;
  }
  if (off == 0xC) { armCtrl = v; return; }
  if (off >= 0x800000 && off < 0x802000) hw[(off - 0x800000) >> 2] = v;
}
void Wii::reply(u32 addr, s32 res, u64 delay) {
  Bus& b = m->bus; u32 cmd = b.r32(addr), fd = b.r32(addr + 8);
  b.w32(addr, 8); b.w32(addr + 4, (u32)res); b.w32(addr + 8, cmd); b.w32(addr + 0xC, fd);
  replies.push_back(addr); if (replies.size() == 1 && !(ppcCtrl & 4)) replyDue = m->cpu.icount + delay;
  updateDue();
}
void Wii::updateDue() {   // tell the idle-loop fast-forward when the next IPC/Bluetooth event is due, so it never skips past it
  u64 d = replies.empty() ? 0 : (ppcCtrl & 4) ? m->cpu.icount + 20000 : replyDue;
  u64 bd = btNextDue(*this); if (bd && (!d || bd < d)) d = bd;
  m->wiiDue = d;
}
void Wii::tick() {
  { // Idle-loop detection: "lwz rX,d(r13); cmpwi rX,0; beq/bne <back to lwz>" is the SDK's wait-for-interrupt spin. Once found, the CPU fast-forwards to the next hardware event
    // instead of executing millions of identical instructions (the games spend ~90% of their time there).
    CPU& c = m->cpu; static bool off = getenv("NOIDLE") != nullptr;
    if (!off && c.nspins < 2 && (c.msr & 0x8000)) {
      for (int back = 0; back <= 8; back += 4) {
        u32 a = c.pc - back; u8* p = m->bus.ptr(a); u8* p1 = m->bus.ptr(a + 8); if (!p || !p1) continue;
        u32 w0 = bswap32(*(u32*)p), w1 = bswap32(*(u32*)(p + 4)), w2 = bswap32(*(u32*)p1);
        if ((w0 >> 26) != 32 || ((w0 >> 16) & 31) != 13) continue;                        // lwz rD,d(r13)
        u32 rd = (w0 >> 21) & 31; if (((w1 >> 26) != 11 && (w1 >> 26) != 10) || ((w1 >> 16) & 31) != rd || (w1 & 0xFFFF) != 0) continue;   // cmpwi/cmplwi rD,0
        if ((w2 >> 26) != 16 || (w2 & 3) != 0 || ((w2 >> 16) & 31) != 2 || (s16)(w2 & 0xFFFC) != -8) continue;                          // b<cond> back to the lwz (cr0.eq)
        u32 bo = (w2 >> 21) & 31; if (bo != 12 && bo != 4) continue;
        bool wz = bo == 12;                                                               // beq: loops while the value is zero
        bool dup = false; for (int k = 0; k < c.nspins; k++) if (c.spins[k].pc == a) dup = true;
        if (!dup) { c.spins[c.nspins++] = {a, (s32)(s16)(w0 & 0xFFFF), wz}; m->log("idle loop detected at %08X (r13%+d, while %s)", a, (s16)(w0 & 0xFFFF), wz ? "zero" : "non-zero"); }
        break;
      }
    }
  }

  if (const char* sc = GENV("WIISCRIPT")) {   // test hook: "vi:mask:dur,vi:mask:dur" presses virtual-pad bits for dur VI frames starting at VI count vi
    u16 b = 0; for (const char* p = sc; *p;) { unsigned long st = strtoul(p, (char**)&p, 10); if (*p == ':') p++; unsigned long mk = strtoul(p, (char**)&p, 0); if (*p == ':') p++; unsigned long du = strtoul(p, (char**)&p, 10); if (*p == ',') p++; if (m->viCount >= st && m->viCount < st + du) b |= (u16)mk; }
    m->padBtn = b; }
  if (GENV("WIIAUTO")) { u64 ph = (m->cpu.icount / 150000000ull) % 4; m->padBtn = (m->padBtn & ~(u16)0x0005) | (ph == 1 ? 0x0001 : ph == 3 ? 0x0004 : 0); }   // test hook: pulse "2" and "A"

  if (GENV("WIISEARCH")) { static bool done = false; if (!done && m->cpu.icount > 200000000ull) { done = true; static const u8 pat[6] = {0x00, 0x19, 0x1D, 0xAA, 0xBB, 0xCC}; static const u8 rev[6] = {0xCC, 0xBB, 0xAA, 0x1D, 0x19, 0x00};
    for (u32 a = 0; a + 6 < RAM_SIZE; a++) { if (!memcmp(m->bus.ram + a, pat, 6)) m->log("SEARCH fwd @%08X", 0x80000000u + a); if (!memcmp(m->bus.ram + a, rev, 6)) m->log("SEARCH rev @%08X", 0x80000000u + a); }
    for (u32 a = 0; a + 6 < MEM2_SIZE; a++) { if (!memcmp(m->bus.ram + MEM2_BASE + a, pat, 6)) m->log("SEARCH fwd @%08X", 0x90000000u + a); } } }

  if (GENV("WIIDBG")) { static u64 nx = 0; if (m->cpu.icount >= nx) { nx = m->cpu.icount + 25000000; m->log("WIIDBG %s replies=%zu ppcCtrl=%X replyDue=%llu wiiDue=%llu nextVi=%llu pc=%08X msr=%X dec=%X decPend=%d pi=%X/%X", btDebug(*this).c_str(), replies.size(), ppcCtrl, (unsigned long long)replyDue, (unsigned long long)m->wiiDue, (unsigned long long)m->nextVi, m->cpu.pc, m->cpu.msr, m->cpu.dec, (int)m->cpu.decPending, m->piIntCause, m->piIntMask); } }
  btTick(*this);
  if (!replies.empty() && !(ppcCtrl & 4) && m->cpu.icount >= replyDue) {
    armMsg = replies.front(); replies.pop_front(); ppcCtrl |= 4; m->updateInts(); replyDue = m->cpu.icount + 3000;
  }
  updateDue();
}
void wiiTick(Machine* m) { m->wii->tick(); }
bool wiiIrq(Machine* m) { return m->wii->irq(); }
void wiiSetRemote(Machine* m, u16 b) { if (m->wii) m->wii->remoteButtons = b; }

// ------------------------------------------------------------ devices
static std::string rdStr(Bus& b, u32 p, u32 max = 128) { std::string s; for (u32 i = 0; i < max; i++) { char c = (char)b.r8(p + i); if (!c) break; s += c; } return s; }
std::string Wii::hostPath(const std::string& p) { std::string r = nandDir; for (char c : p) r += (c == '\\') ? '/' : c; return r; }
static void mkdirs(const std::string& p) { for (size_t i = 1; i < p.size(); i++) if (p[i] == '/') { std::string q = p.substr(0, i); mkdir(q.c_str(), 0755); } }
enum { K_NONE, K_ES, K_FS, K_STM, K_DI, K_FILE, K_NET, K_BT, K_CONTENT };

s32 Wii::devIoctl(Fd& fd, u32 a, bool& defer) {
  Bus& b = m->bus; u32 cmd = b.r32(a + 0xC), in = b.r32(a + 0x10), inl = b.r32(a + 0x14), out = b.r32(a + 0x18), outl = b.r32(a + 0x1C);
  if (trace) m->log("IOCTL %s cmd=%X in=%08X/%X out=%08X/%X", fd.path.c_str(), cmd, in, inl, out, outl);
  switch (fd.kind) {
    case K_FS: {
      auto pathAt = [&](u32 o) { return rdStr(b, o, 64); };
      switch (cmd) {
        case 3: case 9: { std::string p = hostPath(pathAt(in + 6)); struct stat st; if (stat(p.c_str(), &st) == 0) return -105;
          mkdirs(p); if (cmd == 3) return mkdir(p.c_str(), 0755) ? -101 : 0; FILE* f = fopen(p.c_str(), "wb"); if (!f) return -101; fclose(f); return 0; }
        case 5: return 0;
        case 6: { std::string pp = pathAt(in); std::string p = hostPath(pp); struct stat st; if (stat(p.c_str(), &st) != 0) return -106;
          for (u32 i = 0; i < 74 && i < outl; i++) b.w8(out + i, 0); size_t sl = pp.rfind('/'); std::string nm = pp.substr(sl + 1);
          for (size_t i = 0; i < nm.size() && i < 64; i++) b.w8(out + 6 + i, nm[i]); b.w8(out + 71, 3); b.w8(out + 72, 3); b.w8(out + 73, 3); return 0; }
        case 7: { std::string p = hostPath(pathAt(in)); if (remove(p.c_str()) != 0) { if (rmdir(p.c_str()) != 0) return -106; } return 0; }
        case 8: { std::string p1 = hostPath(pathAt(in)), p2 = hostPath(pathAt(in + 64)); mkdirs(p2); return rename(p1.c_str(), p2.c_str()) ? -106 : 0; }
        case 2: for (u32 i = 0; i < outl; i += 4) b.w32(out + i, 0x100); return 0;
      }
      return 0; }
    case K_FILE:
      if (cmd == 0xB) { b.w32(out, (u32)fd.data.size()); b.w32(out + 4, fd.pos); return 0; }
      return -101;
    case K_STM:
      if (cmd == 0x1000) { defer = true; return 0; }   // event hook: completes only on a power/reset event, which never comes
      return 0;
    case K_DI: return 1;
    default: return -4;
  }
}

s32 Wii::devIoctlv(Fd& fd, u32 a, bool& defer) {
  (void)defer;
  Bus& b = m->bus; u32 cmd = b.r32(a + 0xC), nin = b.r32(a + 0x10), nio = b.r32(a + 0x14), vp = b.r32(a + 0x18);
  auto V = [&](u32 i, u32& p, u32& l) { p = b.r32(vp + i * 8); l = b.r32(vp + i * 8 + 4); };
  u32 p0 = 0, l0 = 0, p1 = 0, l1 = 0, p2 = 0, l2 = 0; if (nin + nio > 0) V(0, p0, l0); if (nin + nio > 1) V(1, p1, l1); if (nin + nio > 2) V(2, p2, l2);
  if (trace) m->log("IOCTLV %s cmd=%X in=%u io=%u v0=%08X/%X v1=%08X/%X v2=%08X/%X", fd.path.c_str(), cmd, nin, nio, p0, l0, p1, l1, p2, l2);
  if (fd.kind == K_BT) return btIoctlv(*this, a, defer);
  if (fd.kind == K_ES) {
    switch (cmd) {
      case 0x20: b.w64(p0, titleId); return 0;                                  // GetTitleID
      case 0x07: b.w32(p0, 0x0A0B0C0D); return 0;                              // GetDeviceId
      case 0x1D: { char s[64]; snprintf(s, sizeof s, "/title/%08x/%08x/data", (u32)(titleId >> 32), (u32)titleId); u32 dst = nin ? p1 : p0; for (size_t i = 0; i <= strlen(s); i++) b.w8(dst + i, s[i]); return 0; }   // GetTitleDir
      case 0x09: case 0x24: {   // OpenContent(index) / OpenTitleContent(tid, index)
        u32 idx = cmd == 0x09 ? b.r32(p0) : b.r32(p1);
        Fd c; c.kind = K_CONTENT; c.cidx = idx; if (!readContent(idx, c.data)) { m->log("ES: cannot open content %u: %s", idx, err.c_str()); return -1017; }
        int n = 0x100 + nextFd++; fds[n] = std::move(c); return n; }
      case 0x0A: { u32 cfd = b.r32(p0); auto it = fds.find(cfd); if (it == fds.end()) return -1017; Fd& c = it->second;
        u32 n = std::min<u32>(l1, (u32)c.data.size() - std::min<u32>(c.pos, (u32)c.data.size())); if (n) { for (u32 i = 0; i < n; i++) b.w8(p1 + i, c.data[c.pos + i]); if (g_jit) g_jit->invalidateRange(p1, n); }
        c.pos += n; return (s32)n; }
      case 0x0B: fds.erase(b.r32(p0)); return 0;
      case 0x1B: { for (u32 i = 0; i < l1; i++) b.w8(p1 + i, 0); b.w64(p1 + 16, titleId); b.w16(p1 + 24, 0xFFFF); return 0; }   // DiGetTicketView: unlimited licence
      case 0x16: { u32 po, lo; V(2, po, lo); b.w32(po, 0); return 0; }                                                        // GetConsumption: no limits
      case 0x23: { u32 cfd = b.r32(p0), w = b.r32(p1), md = b.r32(p2); auto it = fds.find(cfd); if (it == fds.end()) return -1017; Fd& c = it->second;
        s64 np = md == 0 ? (s64)w : md == 1 ? (s64)c.pos + (s32)w : (s64)c.data.size() + (s32)w; if (np < 0) np = 0; c.pos = (u32)np; return (s32)c.pos; }
      default: m->log("ES: unhandled ioctlv %X", cmd); return 0;
    }
  }
  if (fd.kind == K_FS) {
    if (cmd == 4) {   // ReadDir
      std::string p = hostPath(rdStr(b, p0, 64)); DIR* d = opendir(p.c_str()); if (!d) return -106;
      std::vector<std::string> names; while (dirent* e = readdir(d)) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; names.push_back(e->d_name); } closedir(d);
      std::sort(names.begin(), names.end());
      if (nin == 1) { b.w32(p1, (u32)names.size()); return 0; }
      u32 mx = b.r32(p1); u32 cnt = std::min<u32>(mx, (u32)names.size()); u32 po, lo, no, ln; V(2, po, lo); V(3, no, ln);
      for (u32 i = 0; i < cnt; i++) for (int k = 0; k < 13; k++) b.w8(no + i * 13 + k, k < (int)names[i].size() && k < 12 ? names[i][k] : 0);
      b.w32(po, cnt); return 0; }
    if (cmd == 0xC) { u32 po, lo; V(1, po, lo); b.w32(po, 0); b.w32(po + 4, 1); return 0; }   // GetUsage
  }
  m->log("unhandled ioctlv on %s cmd %X", fd.path.c_str(), cmd);
  return -4;
}

void Wii::handle(u32 a) {
  Bus& b = m->bus; u32 cmd = b.r32(a), fdn = b.r32(a + 8); s32 res = 0; bool defer = false;
  switch (cmd) {
    case 1: {
      std::string p = rdStr(b, b.r32(a + 0xC)); u32 mode = b.r32(a + 0x10); Fd f; f.path = p; f.mode = mode;
      if (p == "/dev/es") f.kind = K_ES; else if (p == "/dev/fs") f.kind = K_FS; else if (p.rfind("/dev/stm/", 0) == 0) f.kind = K_STM; else if (p == "/dev/di") f.kind = K_DI; else if (p == "/dev/usb/oh1/57e/305") f.kind = K_BT;
      else if (p.rfind("/dev/", 0) == 0) { m->log("IOS open %s -> not available", p.c_str()); res = -6; break; }
      else { f.kind = K_FILE; std::string hp = hostPath(p); const char* fm = "rb";
        FILE* fp = fopen(hp.c_str(), fm); if (!fp) { res = -106; if (trace) m->log("IOS open file %s -> ENOENT", p.c_str()); break; }
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); f.data.resize(n); if (n && fread(f.data.data(), 1, n, fp) != (size_t)n) {} fclose(fp); }
      int n = nextFd++; fds[n] = std::move(f); res = n; if (trace) m->log("IOS open %s -> fd %d", p.c_str(), n); break; }
    case 2: { auto it = fds.find(fdn); if (it != fds.end()) { Fd& f = it->second; if (f.kind == K_FILE && (f.mode & 2)) { std::string hp = hostPath(f.path); if (FILE* fp = fopen(hp.c_str(), "wb")) { fwrite(f.data.data(), 1, f.data.size(), fp); fclose(fp); } } fds.erase(it); } res = 0; break; }
    case 3: case 4: {
      auto it = fds.find(fdn); if (it == fds.end()) { res = -4; break; } Fd& f = it->second; u32 p = b.r32(a + 0xC), n = b.r32(a + 0x10);
      if (f.kind != K_FILE) { res = -4; break; }
      if (cmd == 3) { u32 avail = f.pos < f.data.size() ? (u32)f.data.size() - f.pos : 0; n = std::min(n, avail); for (u32 i = 0; i < n; i++) b.w8(p + i, f.data[f.pos + i]); if (trace) m->log("IOS read %s off=%u n=%u", f.path.c_str(), f.pos, n); f.pos += n; res = n; }
      else { if (f.pos + n > f.data.size()) f.data.resize(f.pos + n); for (u32 i = 0; i < n; i++) f.data[f.pos + i] = b.r8(p + i); f.pos += n; res = n; }
      break; }
    case 5: { auto it = fds.find(fdn); if (it == fds.end()) { res = -4; break; } Fd& f = it->second; s32 w = (s32)b.r32(a + 0xC); u32 whence = b.r32(a + 0x10);
      s64 np = whence == 0 ? w : whence == 1 ? (s64)f.pos + w : (s64)f.data.size() + w; if (np < 0 || np > (s64)f.data.size()) { res = -101; break; } f.pos = (u32)np; res = (s32)f.pos; break; }
    case 6: case 7: {
      auto it = fds.find(fdn); if (it == fds.end()) { res = -4; break; }
      res = cmd == 6 ? devIoctl(it->second, a, defer) : devIoctlv(it->second, a, defer); break; }
    default: res = -4;
  }
  if (!defer) reply(a, res);
}
