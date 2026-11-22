// Bus, minimal hardware (LLE register level), disc access and HLE-style boot.
#include "gc.h"
#include <cstdarg>
#include <cstdlib>
#include <set>

Bus::Bus() {
  ram = (u8*)aligned_alloc(4096, (RAM_SIZE + 4096 + 4095) & ~4095u); memset(ram, 0, RAM_SIZE + 4096);
  aram = (u8*)calloc(1, ARAM_SIZE);
  lc = (u8*)calloc(1, LC_SIZE);
}
Bus::~Bus() { free(ram); free(aram); free(lc); }

bool g_watchOn = false;
static u32 g_unmapped = 0;
static u64 unmapped(Bus& b, u32 ea, int sz, bool wr, u64 v = 0) {
  if (g_unmapped++ < 40) b.m->log("unmapped %s%d ea=%08X pc=%08X v=%llX", wr ? "W" : "R", sz * 8, ea, b.m->cpu.pc, (unsigned long long)v);
  return 0;
}

#define PHYS(ea) ((ea) & 0x1FFFFFFFu)
#define IS_LC(ea) ((ea) >= LC_BASE && (ea) < LC_BASE + LC_SIZE)
#define IS_HW(p) ((p) >= 0x0C000000u && (p) < 0x0C010000u)

u8 Bus::r8(u32 ea) {
  if (IS_LC(ea)) return lc[ea - LC_BASE];
  u32 p = PHYS(ea); if (p < RAM_SIZE) return ram[p];
  if (IS_HW(p)) return (u8)hwRead(p - 0x0C000000u, 1);
  return (u8)unmapped(*this, ea, 1, false);
}
u16 Bus::r16(u32 ea) {
  if (IS_LC(ea)) return bswap16(*(u16*)(lc + (ea - LC_BASE)));
  u32 p = PHYS(ea); if (p < RAM_SIZE) return bswap16(*(u16*)(ram + p));
  if (IS_HW(p)) return (u16)hwRead(p - 0x0C000000u, 2);
  return (u16)unmapped(*this, ea, 2, false);
}
u32 Bus::r32(u32 ea) {
  if (IS_LC(ea)) return bswap32(*(u32*)(lc + (ea - LC_BASE)));
  u32 p = PHYS(ea); if (p < RAM_SIZE) return bswap32(*(u32*)(ram + p));
  if (IS_HW(p)) return hwRead(p - 0x0C000000u, 4);
  return (u32)unmapped(*this, ea, 4, false);
}
u64 Bus::r64(u32 ea) { return ((u64)r32(ea) << 32) | r32(ea + 4); }
void Bus::w8(u32 ea, u8 v) {
  if (IS_LC(ea)) { lc[ea - LC_BASE] = v; return; }
  u32 p = PHYS(ea); if (p < RAM_SIZE) { ram[p] = v; return; }
  if (IS_HW(p)) { hwWrite(p - 0x0C000000u, v, 1); return; }
  unmapped(*this, ea, 1, true, v);
}
void Bus::w16(u32 ea, u16 v) {
  if (IS_LC(ea)) { *(u16*)(lc + (ea - LC_BASE)) = bswap16(v); return; }
  u32 p = PHYS(ea); if (p < RAM_SIZE) { *(u16*)(ram + p) = bswap16(v); return; }
  if (IS_HW(p)) { hwWrite(p - 0x0C000000u, v, 2); return; }
  unmapped(*this, ea, 2, true, v);
}
static u32 g_watch = GENV("WATCH") ? (u32)strtoul(GENV("WATCH"), 0, 16) : 0;
static bool g_wInit = (g_watchOn = g_watch != 0);
void Bus::w32(u32 ea, u32 v) {
  if (__builtin_expect(g_watch != 0, 0) && (ea & ~3u) == g_watch) { static int k = 0; if (k++ < 100000) this->m->log("WATCH w32 %08X <- %08X pc=%08X lr=%08X", ea, v, this->m->cpu.pc, this->m->cpu.lr); }
  if (IS_LC(ea)) { *(u32*)(lc + (ea - LC_BASE)) = bswap32(v); return; }
  u32 p = PHYS(ea); if (p < RAM_SIZE) { *(u32*)(ram + p) = bswap32(v); return; }
  if (IS_HW(p)) { hwWrite(p - 0x0C000000u, v, 4); return; }
  unmapped(*this, ea, 4, true, v);
}
void Bus::w64(u32 ea, u64 v) { w32(ea, (u32)(v >> 32)); w32(ea + 4, (u32)v); }
u8* Bus::ptr(u32 ea) { u32 p = PHYS(ea); return p < RAM_SIZE ? ram + p : nullptr; }

void Machine::log(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%10llu] ", (unsigned long long)cpu.icount);
  vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}

// ---------------- disc -----------------
bool Machine::readDisc(u64 off, void* dst, u32 len) {
  if (!iso) return false;
  if (fseeko(iso, (off_t)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, iso) == len || true;
}

static u32 rd32be(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool Machine::loadIso(const char* path) {
  iso = fopen(path, "rb");
  if (!iso) { fprintf(stderr, "cannot open %s\n", path); return false; }
  u8 h[0x440]; readDisc(0, h, sizeof h);
  if (rd32be(h + 0x1C) != 0xC2339F3D) { fprintf(stderr, "not a GameCube disc image (is it an RVZ/GCZ? convert to ISO first)\n"); return false; }
  u32 fstOff = rd32be(h + 0x424), fstSz = rd32be(h + 0x428);
  fst.resize(fstSz); readDisc(fstOff, fst.data(), fstSz);
  // flatten the FST into a file list (for logging and the future file layer)
  u32 n = rd32be(fst.data() + 8); const u8* names = fst.data() + n * 12;
  std::vector<std::string> dirs; std::vector<u32> dirEnd;
  for (u32 i = 1; i < n; i++) {
    while (!dirEnd.empty() && i >= dirEnd.back()) { dirs.pop_back(); dirEnd.pop_back(); }
    const u8* e = fst.data() + i * 12; u32 no = rd32be(e) & 0xFFFFFF; std::string nm((const char*)names + no);
    if (e[0]) { dirs.push_back(nm); dirEnd.push_back(rd32be(e + 8)); }
    else { std::string p; for (auto& d : dirs) p += d + "/"; files.push_back({p + nm, rd32be(e + 4), rd32be(e + 8)}); }
  }
  return true;
}

// ---------------- boot -----------------
bool Machine::boot() {
  u8 h[0x440]; readDisc(0, h, sizeof h);
  u32 dolOff = rd32be(h + 0x420), fstOff = rd32be(h + 0x424), fstSz = rd32be(h + 0x428), fstMax = rd32be(h + 0x42C);
  u8 d[0x100]; readDisc(dolOff, d, sizeof d);
  u32 bss = rd32be(d + 0xD8), bssSz = rd32be(d + 0xDC), entry = rd32be(d + 0xE0);
  if (bss && bssSz) memset(bus.ram + (bss & 0x1FFFFFFF), 0, bssSz);   // BSS first: it overlaps .sdata, which is loaded after
  for (int i = 0; i < 18; i++) {
    u32 off = rd32be(d + i * 4), addr = rd32be(d + 0x48 + i * 4), sz = rd32be(d + 0x90 + i * 4);
    if (!sz) continue;
    if ((addr & 0x1FFFFFFF) + sz > RAM_SIZE) { log("DOL section out of RAM"); return false; }
    readDisc((u64)dolOff + off, bus.ram + (addr & 0x1FFFFFFF), sz);
  }

  // Low memory globals (what the IPL/apploader would leave behind)
  u8* L = bus.ram;
  auto w32 = [&](u32 o, u32 v) { *(u32*)(L + o) = bswap32(v); };
  memcpy(L, h, 0x20);                    // game id, disc, version, audio streaming
  w32(0x1C, 0xC2339F3D);
  w32(0x20, 0x0D15EA5E); w32(0x24, 1);
  w32(0x28, RAM_SIZE); w32(0x2C, 0x00000001);
  u32 fstAddr = 0x81800000u - ((fstSz + 31) & ~31u);
  u32 bi2Addr = fstAddr - 0x2000;
  u8 bi2[0x2000]; readDisc(0x440, bi2, sizeof bi2);
  memcpy(L + (bi2Addr & 0x1FFFFFFF), bi2, sizeof bi2);
  memcpy(L + (fstAddr & 0x1FFFFFFF), fst.data(), fstSz);
  w32(0x34, bi2Addr);                    // arena hi
  w32(0x38, fstAddr); w32(0x3C, fstMax > fstSz ? fstMax : fstSz);
  w32(0xCC, 1);                          // video mode: PAL
  w32(0xD0, ARAM_SIZE);
  w32(0xE8, RAM_SIZE); w32(0xEC, 0x81800000u); w32(0xF0, RAM_SIZE);
  w32(0xF4, bi2Addr);
  w32(0xF8, 162000000); w32(0xFC, 486000000);

  // Per-game compatibility patches, applied to the in-memory image only (never to the disc or to any shipped file).
  if (!memcmp(h, "GD6P70", 6)) {
    auto patch = [&](u32 addr, u32 expect, u32 val) { u32* p = (u32*)(bus.ram + (addr & 0x1FFFFFFF)); if (bswap32(*p) == expect) *p = bswap32(val); else log("patch %08X skipped (unexpected word %08X)", addr, bswap32(*p)); };
    if (!GENV("NOIDLE")) { cpu.spins[0] = {0x802C86B4, -0x63a8, true}; cpu.spins[1] = {0x80244284, -0x7468, false}; cpu.nspins = 2; }                       // OSIdle loop (lwz r0,-0x63a8(r13); cmplwi; beq)
    patch(0x80249B4C, 0x800D8B98, 0x4E800020);   // GfxSync(): busy-waits for a draw-done callback that this frame never gets; return immediately
  }
  cpu.bus = &bus; cpu.m = this; bus.m = this;
  cpu.pc = entry; cpu.msr = 0x2000;      // FP enabled; the OS configures the rest
  cpu.gpr[1] = 0x816FFFF0;
  log("boot: dol@%X entry=%08X fst@%08X(%X) bi2@%08X", dolOff, entry, fstAddr, fstSz, bi2Addr);
  return true;
}
