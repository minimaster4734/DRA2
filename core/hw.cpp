#include <map>
#include <algorithm>
#include "jit_rt.h"
// Register-level hardware: PI, VI, DI (DVD), DSP handshake, EXI/SI/AI/CP/PE stubs.
#include "gc.h"
#include "gl.h"
#include <set>
#include <cstdlib>

bool g_romBooted();
static std::set<u64> g_seen; static int g_seenCount = 0;
static void seen(Machine* m, const char* dev, u32 off, bool wr, u32 v, int sz) {
  if (GENV("SILOG") && dev[0]=='S' && dev[1]=='I') { static int k=0; static u64 from = GENV("SIFROM") ? strtoull(GENV("SIFROM"),0,10)*1000000ull : 0; if (m->cpu.icount >= from && k++<300) m->log("SI %s%d +%04X %08X pc=%08X", wr?"W":"R", sz*8, off, v, m->cpu.pc); }
  u64 key = ((u64)off << 8) | (wr ? 1 : 0) | (sz << 1);
  if (g_seenCount < 400 && g_seen.insert(key).second) {
    g_seenCount++;
    m->log("%-4s %s%d +%04X %s%08X pc=%08X", dev, wr ? "W" : "R", sz * 8, off, wr ? "<-" : "->", v, m->cpu.pc);
  }
}

bool wiiIrq(Machine* m);
void Machine::updateInts() {
  u32 c = 0;
  // DI: status bits 2,4,6 with masks 1,3,5
  u32 sr = diReg[0];
  if (((sr >> 2) & (sr >> 1) & 1) || ((sr >> 4) & (sr >> 3) & 1) || ((sr >> 6) & (sr >> 5) & 1)) c |= INT_DI;
  for (int i = 0; i < 4; i++) { u32 hi = viReg[(0x30 + i * 4) >> 1]; if ((hi & 0x8000) && (hi & 0x1000)) c |= INT_VI; }
  { u32 d = dspReg[0x0A >> 1];
    if (((d & 0x08) && (d & 0x10)) || ((d & 0x20) && (d & 0x40)) || ((d & 0x80) && (d & 0x100))) c |= INT_DSP; }
  { u32 csr = siReg[13]; csr = (csr & ~(1u << 28)) | ((siReg[14] & 0x20202020u) ? 1u << 28 : 0);
    if (((csr >> 31) & (csr >> 30) & 1) || ((csr >> 28) & (csr >> 27) & 1)) c |= INT_SI; }
  { u32 pe = peReg[0x0A >> 1]; if ((pe & 8) && (pe & 2)) c |= INT_PE_FINISH; if ((pe & 4) && (pe & 1)) c |= INT_PE_TOKEN; }
  for (int ch = 0; ch < 3; ch++) {
    u32 s = exiReg[ch][0];
    if (((s & 2) && (s & 1)) || ((s & 8) && (s & 4)) || ((s & 0x800) && (s & 0x400))) c |= INT_EXI;
  }
  if (wii && wiiIrq(this)) c |= 1u << 14;
  piIntCause = c;
}

// ---- EXI: channel 0 dev1 = RTC/SRAM, everything else reports "no device" ----
static u8 g_sram[64];
static bool g_sramInit = false;
static void sramInit() {
  if (g_sramInit) return; g_sramInit = true;
  memset(g_sram, 0, 64);
  g_sram[0x12] = 0;      // language: English
  g_sram[0x13] = 0x05;   // PAL, stereo
  for (int c = 0; c < 2; c++) {   // SRAM-ex: per-slot 12-byte flash ID + checksum (~sum), as the card library verifies it
    u32 sum = 0; for (int i = 0; i < 12; i++) { u8 v = (u8)(0x5A + 0x1D * i + 0x33 * c); g_sram[0x18 + 12 * c + i] = v; sum += v; }
    g_sram[0x18 + 0x26 + c] = (u8)~sum; }
  u16 chk = 0, chk2 = 0;
  for (int i = 0x0C; i < 0x14; i += 2) { u16 w = g_sram[i] << 8 | g_sram[i + 1]; chk += w; chk2 += (u16)(w ^ 0xFFFF); }
  g_sram[0] = chk >> 8; g_sram[1] = chk; g_sram[2] = chk2 >> 8; g_sram[3] = chk2;
}

// ---- Memory Card (slot A) behind EXI channel 0 / CS0: byte-level flash-card protocol, backed by a raw image file ----
struct MemCard {
  bool init = false, present = false, dirty = false; std::string path; std::vector<u8> data;
  u32 idv = 0x0010;                 // size in Mbit: 16 Mbit = 251 blocks
  u8 status = 0xC1;                 // not busy (0x80), unlocked (0x40), ready (0x01)
  bool intEn = false; bool kick = false;   // kick: a write/erase finished -> raise the card interrupt
  int cmd = -1; u32 argc = 0; u8 arg[8]; u32 addr = 0, outPos = 0; u64 lastFlush = 0;
  void open() {
    if (init) return; init = true;
    if (getenv("NOCARD")) return;
    const char* p = getenv("DRA2_CARD"); path = p ? p : "dra2_card_a.raw";
    data.assign((size_t)idv * 0x20000, 0xFF);
    if (FILE* f = fopen(path.c_str(), "rb")) { size_t n = fread(data.data(), 1, data.size(), f); (void)n; fclose(f); }
    present = true;
  }
  void flush() {
    if (!dirty || !present) return; dirty = false;
    std::string tmp = path + ".tmp"; if (FILE* f = fopen(tmp.c_str(), "wb")) { fwrite(data.data(), 1, data.size(), f); fclose(f); rename(tmp.c_str(), path.c_str()); }
  }
  void select() { if ((cmd == 0xF4 || cmd == 0xF2) && argc > 4) kick = true; cmd = -1; argc = 0; }
  static u32 decode(const u8* a) { return ((u32)(a[0] & 0x60) << 17) | ((u32)a[1] << 9) | ((u32)(a[2] & 3) << 7) | (a[3] & 0x7F); }
  void feed(u8 b) {
    if (cmd < 0) { cmd = b; argc = 0; outPos = 0; if (getenv("CARDTRACE")) fprintf(stderr, "[card] cmd %02X\n", b); return; }
    if (argc < 8) arg[argc] = b;
    argc++;
    if (cmd == 0x81 && argc == 1) { intEn = b != 0; if (getenv("CARDTRACE")) fprintf(stderr, "[card] interrupts %s\n", intEn ? "on" : "off"); }
    if ((cmd == 0xF4 || cmd == 0xF2) && argc > 4) { u32 a = addr + (argc - 5); if (a < data.size()) { data[a] = b; dirty = true; } }
    if ((cmd == 0xF4 || cmd == 0xF2) && argc == 4) { addr = decode(arg); if (getenv("CARDTRACE")) fprintf(stderr, "[card] program @%06X\n", addr); }
    if (cmd == 0xF1 && argc == 2) { addr = ((u32)arg[0] << 17) | ((u32)arg[1] << 9); addr &= ~0x1FFFu; if (addr + 0x2000 <= data.size()) { memset(&data[addr], 0xFF, 0x2000); dirty = true; } kick = true;
      if (getenv("CARDTRACE")) fprintf(stderr, "[card] erase @%06X\n", addr); }
    if (cmd == 0x52 && argc == 4) { addr = decode(arg); if (getenv("CARDTRACE")) fprintf(stderr, "[card] read @%06X\n", addr); }
  }
  u8 out() {
    switch (cmd) {
      case 0x00: { u32 i = outPos++; return i < 4 ? (u8)(idv >> (8 * (3 - i))) : 0; }
      case 0x83: return status;
      case 0x52: { if (argc < 8) return 0; u32 a = addr + outPos++; return a < data.size() ? data[a] : 0xFF; }
      default: return 0;
    }
  }
};
static MemCard g_card;
void flushMemCard() { g_card.flush(); }
static struct { u32 cmd; int phase; u32 pos; } g_exi[3];

u32 Machine::exiRead(u32 off) {
  u32 ch = off / 0x14, r = (off % 0x14) / 4;
  if (ch > 2) return 0;
  g_card.open();
  u32 v = exiReg[ch][r];
  if (ch == 0 && r == 0 && g_card.present) v |= 0x1000;   // EXT: a card is inserted
  return v;
}

void Machine::exiWrite(u32 off, u32 val) {
  u32 ch = off / 0x14, r = (off % 0x14) / 4;
  if (ch > 2) return;
  u32* R = exiReg[ch];
  if (r == 0) {  // CSR: bits 1,3,11 write-1-to-clear
    u32 clr = val & 0x80A; u32 keep = R[0] & (clr | ~0x80Au); (void)keep;
    R[0] = (R[0] & ~clr & ~0x7FFu & ~0x2000u) | (R[0] & 0x1000u) | (val & ~0x80Au & 0x27FFu) ;
    R[0] = (R[0] & ~0x80Au) | (R[0] & 0x80A & ~clr);
    if (!(R[0] & 0x380)) { g_exi[ch].phase = 0; if (ch == 0) { g_card.select(); if (g_card.kick) { g_card.kick = false; if (g_card.intEn) cardIntDue = cpu.icount + 30000; } } }
    updateInts(); return;
  }
  if (r != 3) { R[r] = val; return; }
  R[3] = val & ~1u;
  if (!(val & 1)) return;
  u32 type = (val >> 2) & 3, len = ((val >> 4) & 3) + 1; bool dma = val & 2;
  u32 sel = (R[0] >> 7) & 7;
  auto& st = g_exi[ch];
  if (dma) len = R[2];
  if (ch == 0 && sel == 2) {            // CS1 = RTC/SRAM
    sramInit();
    if (st.phase == 0) {
      st.cmd = R[4]; st.phase = 1; st.pos = 0;
      if ((st.cmd >> 24) == 0x20 && (st.cmd & 0x00FFFF00) == 0x000100) st.pos = ((st.cmd >> 6) & 0xFF);  // SRAM offset
      if (st.cmd & 0x80000000u) st.phase = 2;
    } else if (type == 0) {              // read
      if ((st.cmd & 0xFFFFFF00) == 0x20000100 || (st.cmd & 0x3FFFFF00) == 0x20000100) {
        u32 o = ((st.cmd >> 6) & 0x3FF) ? ((st.cmd >> 6) & 0xFF) : 0;
        if (dma) { for (u32 i = 0; i < len; i++) bus.ram[(R[1] & 0x1FFFFFFF) + i] = g_sram[(o + i) & 63]; }
        else { u32 v = 0; for (u32 i = 0; i < len; i++) v = (v << 8) | g_sram[(o + i) & 63]; R[4] = v << (8 * (4 - len)); }
      } else if ((st.cmd >> 24) == 0x20 && ((st.cmd >> 8) & 0xFFFF) == 0) {       // RTC counter
        R[4] = 0x2A000000 + (u32)(cpu.icount / 400000);
      } else R[4] = 0;
    }
  } else if (ch == 0 && sel == 1 && (g_card.open(), g_card.present)) {   // CS0 = memory card
    if (getenv("CARDTRACE")) { fprintf(stderr, "[card] xfer type=%u len=%u dma=%d R4=%08X bt:", type, len, (int)dma, R[4]); u32 sp = cpu.gpr[1]; for (int k = 0; k < 8 && sp > 0x80000000u && sp < 0x81800000u; k++) { u32 nx = bus.r32(sp); fprintf(stderr, " %08X", bus.r32(nx + 4)); sp = nx; } fputc(10, stderr); }
    if (dma) {
      u8* p = bus.ram + (R[1] & 0x1FFFFFFF);
      if (type == 1) for (u32 i = 0; i < len; i++) g_card.feed(p[i]); else for (u32 i = 0; i < len; i++) p[i] = g_card.out();
    } else if (type == 1) { for (u32 i = 0; i < len; i++) g_card.feed((u8)(R[4] >> (24 - 8 * i))); }
    else { u32 v = 0; for (u32 i = 0; i < len; i++) v = (v << 8) | g_card.out(); R[4] = v << (8 * (4 - len)); }
  } else if (type == 0) {                // reads from absent devices
    if (dma) memset(bus.ram + (R[1] & 0x1FFFFFFF), 0, len); else R[4] = 0;
  }
  R[0] |= 8;                             // transfer complete
  updateInts();
}

void wiiTick(Machine* m);
void Machine::tick() {
  if (GENV("PCPROF")) { static std::map<u32, u32> h; static u64 nxt = 0, n = 0; h[cpu.pc & ~0xFu]++; n++;   // sampled once per slice (every 4096 guest instructions)
    if (cpu.icount > nxt) { nxt = cpu.icount + 1000000000ull; std::vector<std::pair<u32, u32>> v; for (auto& kv : h) v.push_back({kv.second, kv.first}); std::sort(v.rbegin(), v.rend()); fprintf(stderr, "PCPROF @%lluM:", (unsigned long long)(cpu.icount / 1000000)); for (size_t k = 0; k < v.size() && k < 8; k++) fprintf(stderr, " %08X=%u%%", v[k].second, v[k].first * 100 / (u32)n); fprintf(stderr, "\n"); h.clear(); n = 0; } }

  if (wii) wiiTick(this);
  gxProcess();
  if (cardIntDue && cpu.icount >= cardIntDue) { cardIntDue = 0; exiReg[0][0] |= 2; updateInts(); }
  if (g_card.dirty && cpu.icount - g_card.lastFlush > 600000000ull) { g_card.lastFlush = cpu.icount; g_card.flush(); }
  { u32 ctl = dspReg[0x36 >> 1]; u32 units = ctl & 0x7FFF;
    if ((ctl & 0x8000) && units) {
      if (!aiDmaDue) aiDmaDue = cpu.icount + (u64)units * 75000;
      else if (cpu.icount >= aiDmaDue) {
        u32 addr = (((u32)dspReg[0x30 >> 1] << 16) | dspReg[0x32 >> 1]) & 0x1FFFFFE0;
        if (inRamPhys(addr, units * 32)) { const u8* s = bus.ram + addr; size_t o = audioOut.size(); audioOut.resize(o + units * 16);
          for (u32 i = 0; i < units * 8; i++) { audioOut[o + i * 2] = (s16)((s[i * 4 + 2] << 8) | s[i * 4 + 3]); audioOut[o + i * 2 + 1] = (s16)((s[i * 4] << 8) | s[i * 4 + 1]); } }   // hardware order is R,L
        aiDmaCount++; dspReg[0x0A >> 1] |= 8; aiDmaDue = 0; updateInts();
      }
    } else aiDmaDue = 0; }
  if (axYieldDue && cpu.icount >= axYieldDue && !(dspMailToCpu & 0x80000000u)) { axYieldDue = 0; dspPushMail(0xDCD10002u, true); }
  if (finishDue && cpu.icount >= finishDue) { finishDue = 0; peReg[0x0A >> 1] |= 8; updateInts(); }
  if (dspRearm && cpu.icount >= dspRearm) { dspRearm = 0; if (!g_romBooted() && !(dspMailToCpu & 0x80000000u)) dspPushMail(0x0071FEED); }
  if (cpu.icount >= nextVi) {
    nextVi += 6000000; viCount++; if (rd) rd->vblank();
    static std::vector<std::pair<u64, u16>> script; static bool loaded = false;
    if (!loaded) { loaded = true; if (const char* s = GENV("PADSCRIPT")) { std::string str = s; size_t p = 0; while (p < str.size()) { size_t q = str.find(',', p); if (q == std::string::npos) q = str.size();
        std::string it = str.substr(p, q - p); size_t c = it.find(':'); if (c != std::string::npos) script.push_back({strtoull(it.c_str(), 0, 10), (u16)strtoul(it.c_str() + c + 1, 0, 0)}); p = q + 1; } } }
    for (auto& s : script) if (s.first == viCount) padBtn = s.second;
    siPoll();
    for (int i = 0; i < 4; i++) { u16& hi = viReg[(0x30 + i * 4) >> 1]; if (hi & 0x1000) hi |= 0x8000; }
    updateInts();
  }
}

bool dvdCommand(Machine& m);

void Machine::diExec(u32) {
  u32 cmd = diReg[2] >> 24; u32 mar = diReg[5], len = diReg[6];
  u32 dma = diReg[7] & 2;
  switch (cmd) {
    case 0xA8: case 0xA9: {  // read (A9 = alt)
      u64 off = (u64)diReg[3] << 2;
      if (trace || GENV("DVDLOG")) { const char* nm = "?"; u32 fo = 0; for (auto& f : files) if (off >= f.off && off < (u64)f.off + f.size) { nm = f.path.c_str(); fo = (u32)(off - f.off); break; }
        log("DVD read %08llX len=%X -> %08X  %s+%X", (unsigned long long)off, len, mar, nm, fo); }
      if (dma && len) { if ((mar & 0x1FFFFFFF) + len <= RAM_SIZE) { readDisc(off, bus.ram + (mar & 0x1FFFFFFF), len); if (g_jit) g_jit->invalidateRange(mar, len); } }
      break; }
    case 0x12: { // drive inquiry
      u8 info[0x20] = {0, 0, 0x00, 0x20, 0x20, 0x02, 0x04, 0x02};
      if (dma && len) memcpy(bus.ram + (mar & 0x1FFFFFFF), info, len > 0x20 ? 0x20 : len);
      break; }
    default: if (trace || GENV("DVDLOG")) log("DI cmd %08X %08X %08X len=%X mar=%08X cr=%X", diReg[2], diReg[3], diReg[4], len, mar, diReg[7]); break;
  }
  diReg[6] = 0; diReg[7] &= ~1u; diReg[0] |= 0x10;  // TC interrupt
  updateInts();
}

u32 Bus::hwRead(u32 off, int size) {
  Machine* mm = m; u32 v = 0;
  u32 blk = off & 0xF000;
  switch (blk) {
    case 0x0000: v = off == 0 ? 0xC : mm->cpReg[(off >> 1) & 0x7F]; if (off == 0 && size == 2) v = 0xC; seen(mm, "CP", off, 0, v, size); break;
    case 0x1000: v = mm->peReg[(off >> 1) & 0x7F]; seen(mm, "PE", off, 0, v, size); break;
    case 0x2000: {
      u32 r = off & 0xFF;
      if (size == 4) v = (mm->viReg[r >> 1] << 16) | mm->viReg[(r >> 1) + 1];
      else if (size == 2) v = mm->viReg[r >> 1];
      else v = (mm->viReg[r >> 1] >> ((r & 1) ? 0 : 8)) & 0xFF;
      if (r == 0x2C && size == 2) v = (mm->cpu.icount / 100000) & 0x3FF; // vertical position
      seen(mm, "VI", off, 0, v, size); break; }
    case 0x3000:
      switch (off & 0xFF) {
        case 0x00: v = mm->piIntCause | 0x10000 ; break;
        case 0x04: v = mm->piIntMask; break;
        case 0x2C: v = 0x20000000 | 0x246500B1; break;  // chip revision
        default: v = mm->shadow[(off >> 2) & 0xFFF];
      }
      seen(mm, "PI", off, 0, v, size); break;
    case 0x4000: v = mm->miReg[(off >> 1) & 0x7F]; if (size == 4) v = (mm->miReg[(off >> 1) & 0x7F] << 16) | mm->miReg[((off >> 1) & 0x7F) + 1]; seen(mm, "MI", off, 0, v, size); break;
    case 0x5000: {
      if (size == 4) { u32 hi = hwRead(off, 2), lo = hwRead(off + 2, 2); v = hi << 16 | lo; break; }
      u32 r = off & 0xFF;
      if (GENV("DSPDBG") && (r == 4 || r == 6)) { static int k = 0; if (k++ < 12) mm->log("DSP mailbox read +%02X pc=%08X mail=%08X", r, mm->cpu.pc, mm->dspMailToCpu); }
      if (r == 0x04 || r == 0x06) { v = r == 4 ? (mm->dspMailToCpu >> 16) : (mm->dspMailToCpu & 0xFFFF); if (r == 0x06) { mm->dspMailToCpu &= 0x7FFFFFFFu; if (!g_romBooted()) mm->dspRearm = mm->cpu.icount + 2000; } }
      else if (r == 0x00 || r == 0x02) { v = r == 0 ? (mm->dspMailToDsp >> 16) : (mm->dspMailToDsp & 0xFFFF); }
      else if (r == 0x16) v = 1;   // AR_MODE: ARAM controller init complete
      else v = mm->dspReg[r >> 1];
      seen(mm, "DSP", off, 0, v, size); break; }
    case 0x6000: {
      u32 sub = off & 0xFC00;
      if (sub == 0x6000) { v = mm->diReg[(off & 0x3F) >> 2]; seen(mm, "DI", off, 0, v, size); }
      else if (sub == 0x6400) { v = mm->siRead(off & 0xFF); seen(mm, "SI", off, 0, v, size); }
      else if (sub == 0x6800) { v = mm->exiRead(off & 0xFF); seen(mm, "EXI", off, 0, v, size); }
      else {
        u32 r = (off >> 2) & 7; v = mm->aiReg[r];
        if (r == 2) v = mm->aiCountBase + ((mm->aiReg[0] & 1) ? (u32)((mm->cpu.icount - mm->aiStart) / 6750) : 0);  // ~48 kHz
        seen(mm, "AI", off, 0, v, size); }
      break; }
    default:
      if (blk == 0x6000 + 0x0C00 || (off & 0xFC00) == 0x6C00) { v = mm->aiReg[(off >> 2) & 7]; seen(mm, "AI", off, 0, v, size); }
      else { v = mm->shadow[(off >> 2) & 0xFFF]; seen(mm, "HW?", off, 0, v, size); }
  }
  if (size == 1) v &= 0xFF; else if (size == 2) v &= 0xFFFF;
  return v;
}

void Bus::hwWrite(u32 off, u32 val, int size) {
  Machine* mm = m; u32 blk = off & 0xF000;
  if (GENV("HWALL") && (blk == 0x5000 || (blk == 0x6000 && (off & 0xFC00) == 0x6C00))) { static int kk = 0; if (kk++ < 500) mm->log("HW W%d +%04X <- %08X pc=%08X", size * 8, off, val, mm->cpu.pc); }
  if (size == 4 && val == 0x45000002u && GENV("DONELOG")) { Machine* q = mm; u32 sp = q->cpu.gpr[1]; q->log("DRAWDONE pc=%08X lr=%08X bt=%08X %08X %08X", q->cpu.pc, q->cpu.lr, q->bus.r32(q->bus.r32(sp)+4), q->bus.r32(q->bus.r32(q->bus.r32(sp))+4), q->bus.r32(q->bus.r32(q->bus.r32(q->bus.r32(sp)))+4)); }
  if (off >= 0x8000) { for (int i = size - 1; i >= 0; i--) mm->gxfifo.push_back((u8)(val >> (8 * i))); { static int lg=0; size_t L = mm->gxfifo.size(); if (GENV("WGPC") && mm->cpu.icount > strtoull(GENV("WGPC"),0,10) && L >= 2 && lg < 60 && mm->cpu.lr != 0x802450B4 && ((mm->gxfifo[L-2] == 0x08 && (mm->gxfifo[L-1] == 0xA0 || mm->gxfifo[L-1] == 0xB0 || mm->gxfifo[L-1] == 0x71)) )) { lg++; mm->log("WGPC reg=%02X pc=%08X lr=%08X", mm->gxfifo[L-1], mm->cpu.pc, mm->cpu.lr); } } return; }
  seen(mm, blk == 0x0000 ? "CP" : blk == 0x1000 ? "PE" : blk == 0x2000 ? "VI" : blk == 0x3000 ? "PI" : blk == 0x4000 ? "MI" : blk == 0x5000 ? "DSP" : "IO", off, 1, val, size);
  switch (blk) {
    case 0x0000: mm->cpReg[(off >> 1) & 0x7F] = (u16)val; break;
    case 0x1000: {
      u32 r = (off & 0xFF) >> 1;
      if (r == (0x0A >> 1)) { u16 old = mm->peReg[r]; mm->peReg[r] = (u16)((val & 3) | (old & 0xC & ~val)); mm->updateInts(); }
      else mm->peReg[(off >> 1) & 0x7F] = (u16)val;
      break; }
    case 0x2000: {
      u32 r = off & 0xFF;
      auto wr16 = [&](u32 reg, u32 v16) {
        if (reg >= 0x30 && reg < 0x40 && (reg & 2) == 0) {            // DI hi: bit15 status cleared by writing 0
          u16 old = mm->viReg[reg >> 1];
          mm->viReg[reg >> 1] = (u16)((v16 & 0x7FFF) | (old & v16 & 0x8000));
        } else mm->viReg[reg >> 1] = (u16)v16;
      };
      if (size == 4) { wr16(r, val >> 16); wr16(r + 2, val & 0xFFFF); } else if (size == 2) wr16(r, val);
      mm->updateInts();
      break; }
    case 0x3000:
      switch (off & 0xFF) {
        case 0x00: break;  // clear on write is device-driven
        case 0x04: mm->piIntMask = val; break;
        default: mm->shadow[(off >> 2) & 0xFFF] = val;
      }
      break;
    case 0x4000: if (size == 4) { mm->miReg[(off >> 1) & 0x7F] = val >> 16; mm->miReg[((off >> 1) & 0x7F) + 1] = (u16)val; } else mm->miReg[(off >> 1) & 0x7F] = (u16)val; break;
    case 0x5000: {
      if (size == 4) { hwWrite(off, val >> 16, 2); hwWrite(off + 2, val & 0xFFFF, 2); break; }
      u32 r = off & 0xFF;
      if (r == 0x2A) {   // ARAM DMA: MRAM addr @0x20, ARAM addr @0x24, count+dir @0x28
        u32 mmAddr = (mm->dspReg[0x20 >> 1] << 16 | mm->dspReg[0x22 >> 1]) & 0x1FFFFFFF;
        u32 arAddr = (mm->dspReg[0x24 >> 1] << 16 | mm->dspReg[0x26 >> 1]) & (ARAM_SIZE - 1);
        u32 cnt = ((mm->dspReg[0x28 >> 1] << 16) | (val & 0xFFFF)); bool toMain = cnt >> 31; cnt &= 0x7FFFFFFF;
        if (cnt && mmAddr + cnt <= RAM_SIZE && arAddr + cnt <= ARAM_SIZE) {
          if (toMain) memcpy(mm->bus.ram + mmAddr, mm->bus.aram + arAddr, cnt); else memcpy(mm->bus.aram + arAddr, mm->bus.ram + mmAddr, cnt);
        }
        mm->dspReg[0x28 >> 1] = 0; mm->dspReg[0x2A >> 1] = 0;
        mm->dspReg[0x0A >> 1] |= 0x20;   // ARINT
        mm->updateInts();
        break;
      }
      if (r == 0x00) mm->dspMailToDsp = (mm->dspMailToDsp & 0xFFFF) | (val << 16);
      else if (r == 0x02) { mm->dspMailToDsp = (mm->dspMailToDsp & 0xFFFF0000) | (val & 0xFFFF); mm->dspMailIn(mm->dspMailToDsp); mm->dspMailToDsp &= 0x7FFFFFFFu; }   // DSP consumed it: clear the pending flag
      else if (r == 0x0A) {  // DSP control. bit0 RESET (self-clearing), 3/5/7 int status (W1C), 1/2/4/6/8/11 r/w
        u32 old = mm->dspReg[0x0A >> 1];
        u32 nv = (old & 0xA8 & ~(val & 0xA8)) | (val & 0x0956);
        if ((old & 0x800) && !(val & 0x800)) { mm->dspMailToCpu = 0x8071FEED; mm->dspBooted = true; }  // DSP ROM boot done
        mm->dspReg[0x0A >> 1] = (u16)nv; mm->updateInts();
      } else mm->dspReg[r >> 1] = (u16)val;
      break; }
    case 0x6000: {
      u32 sub = off & 0xFC00;
      if (sub == 0x6000) {
        u32 r = (off & 0x3F) >> 2;
        if (r == 0) { u32 clr = val & 0x54; mm->diReg[0] = (mm->diReg[0] & ~0x2Au & ~clr) | (val & 0x2A); mm->updateInts(); }
        else if (r == 7) { mm->diReg[7] = val & 7; if (val & 1) mm->diExec(val); }
        else mm->diReg[r] = val;
      } else if (sub == 0x6400) mm->siWrite(off & 0xFF, val);
      else if (sub == 0x6800) mm->exiWrite(off & 0xFF, val);
      else {
        u32 r = (off >> 2) & 7;
        if (r == 0) {  // AICR: bit3 AIINT W1C, bit5 SCRESET
          u32 old = mm->aiReg[0];
          if ((val & 1) && !(old & 1)) { mm->aiStart = mm->cpu.icount; }
          if (!(val & 1) && (old & 1)) { mm->aiCountBase += (u32)((mm->cpu.icount - mm->aiStart) / 6750); }
          if (val & 0x20) { mm->aiCountBase = 0; mm->aiStart = mm->cpu.icount; }
          mm->aiReg[0] = (val & ~0x28u) | (old & 0x08 & ~val);
        } else mm->aiReg[r] = val;
      }
      break; }
    default:
      if ((off & 0xFC00) == 0x6C00) mm->aiReg[(off >> 2) & 7] = val; else mm->shadow[(off >> 2) & 0xFFF] = val;
  }
}

// ---- DSP HLE: boot-ROM mail protocol, then ucode-specific handlers (to be filled in per ucode) ----
static u32 crc32(const u8* p, size_t n) {
  u32 c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
  return ~c;
}
static struct { int phase = 0; u32 cmd = 0; u32 mram = 0, iramDst = 0, len = 0, dram = 0, start = 0; bool booted = false; u32 ucodeCrc = 0; } g_rom;

void Machine::dspPushMail(u32 mail, bool irq) { dspMailToCpu = mail | 0x80000000u; if (irq) { dspReg[0x0A >> 1] |= 0x80; updateInts(); } }

void Machine::dspMailIn(u32 mail) {
  static int n = 0;
  if (n++ < 120) log("DSP mail CPU->DSP %08X (ucode %s)", mail, g_rom.booted ? "running" : "ROM");
  if (g_rom.booted) {   // AX ucode protocol
    static int state = 0; static u32 listWords = 0;   // 0 = expect command, 1 = expect list address
    if (state == 1) { state = 0; axRunList(mail & 0x1FFFFFFF, listWords); axYieldDue = cpu.icount + 30000; return; }
    if ((mail >> 16) == 0xBABE) { state = 1; listWords = mail & 0xFFFF; return; }
    if ((mail >> 16) == 0xCDD1) {
      switch (mail & 0xFFFF) {
        case 0: dspPushMail(0xDCD10001u, true); break;   // MAIL_RESUME -> DSP_RESUME
        case 2: break;                                    // MAIL_RESET: back to ROM; not needed by this game
        default: break;                                   // NEW_UCODE / CONTINUE
      }
    }
    return;
  }
  if (!g_rom.booted) {
    if (g_rom.phase == 0) { if ((mail >> 16) == 0x80F3) { g_rom.cmd = mail & 0xFFFF; g_rom.phase = 1; } return; }
    g_rom.phase = 0;
    switch (g_rom.cmd) {
      case 0xA001: g_rom.mram = mail & 0x1FFFFFFF; break;
      case 0xC002: g_rom.iramDst = mail & 0xFFFF; break;
      case 0xA002: g_rom.len = mail & 0xFFFF; break;
      case 0xB002: g_rom.dram = mail & 0xFFFF; break;
      case 0xD001: {
        g_rom.start = mail & 0xFFFF; g_rom.booted = true;
        if (g_rom.mram + g_rom.len <= RAM_SIZE) g_rom.ucodeCrc = crc32(bus.ram + g_rom.mram, g_rom.len);
        log("DSP ROM: ucode boot: mram=%08X iram@%04X len=%04X start=%04X crc32=%08X", g_rom.mram, g_rom.iramDst, g_rom.len, g_rom.start, g_rom.ucodeCrc);
        if (GENV("DSPDUMP")) { char fn[64]; snprintf(fn, sizeof fn, "dsp_ucode_%08X.bin", g_rom.ucodeCrc); FILE* f = fopen(fn, "wb"); if (f) { fwrite(bus.ram + g_rom.mram, 1, g_rom.len, f); fclose(f); } }
        dspPushMail(0xDCD10000, true);   // DSP_INIT: sent by the AX family of ucodes when they come up
        break; }
      default: break;
    }
  }
}
bool g_romBooted() { return g_rom.booted; }

// ---------------------------------------------------------------- SI (serial interface / controllers)
static void padResponse(Machine& m, u8 cmd, u8* out, int& n) {
  u8 b0 = m.padBtn & 0xFF, b1 = ((m.padBtn >> 8) & 0xFF) | 0x80;
  switch (cmd) {
    case 0x00: case 0xFF: out[0] = 0x09; out[1] = 0x00; out[2] = 0x03; n = 3; break;           // standard GC controller
    case 0x41: case 0x42: out[0] = 0; out[1] = 0x80; for (int i = 0; i < 6; i++) out[2 + i] = i < 4 ? 128 : 0; out[8] = out[9] = 0; n = 10; break;  // origin / calibrate
    default: {  // 0x40 poll (mode 3: sticks, c-stick, analog L/R)
      out[0] = b0; out[1] = b1; for (int i = 0; i < 4; i++) out[2 + i] = m.padStick[i];
      out[6] = (m.padBtn & 0x4000) ? 0xFF : 0; out[7] = (m.padBtn & 0x2000) ? 0xFF : 0; n = 8; }
  }
}

u32 Machine::siRead(u32 off) {
  u32 i = off >> 2;
  if (i >= 0x20) return siReg[i & 0x3F];
  if (i == 14) return siReg[14];
  if (i == 13) return (siReg[13] & ~(1u << 28)) | ((siReg[14] & 0x20202020u) ? 1u << 28 : 0);
  u32 v = siReg[i];
  if (i < 12 && (i % 3) == 1) { int ch = i / 3; siReg[14] &= ~(0x20u << (24 - 8 * ch)); updateInts(); }   // reading INBUFH acknowledges the poll
  return v;
}

void Machine::siWrite(u32 off, u32 val) {
  u32 i = off >> 2;
  if (i >= 0x20) { siReg[i & 0x3F] = val; return; }
  if (i == 13) {
    u32 old = siReg[13];
    u32 keep = val & ~(0x80000000u | 0x20000000u | 1u | (1u << 28));
    siReg[13] = (keep | (old & 0x20000000u & ~val)) | ((old & 0x80000000u) & ~(val & 0x80000000u));
    if (val & 1) siTransfer();
    updateInts(); return;
  }
  if (i == 14) { siReg[14] &= ~(val & 0x0F0F0F0Fu); updateInts(); return; }
  siReg[i] = val;
}

void Machine::siTransfer() {
  u32 csr = siReg[13]; int ch = (csr >> 1) & 3;
  u8 buf[128]; for (int k = 0; k < 32; k++) { u32 w = siReg[0x20 + k]; buf[k * 4] = w >> 24; buf[k * 4 + 1] = w >> 16; buf[k * 4 + 2] = w >> 8; buf[k * 4 + 3] = w; }
  int inLen = (csr >> 8) & 0x7F; if (!inLen) inLen = 128;
  if (ch == 0) {
    u8 resp[128] = {0}; int n = 0; padResponse(*this, buf[0], resp, n);
    memset(buf, 0, sizeof buf); memcpy(buf, resp, std::min(n, inLen));
    for (int k = 0; k < 32; k++) siReg[0x20 + k] = buf[k * 4] << 24 | buf[k * 4 + 1] << 16 | buf[k * 4 + 2] << 8 | buf[k * 4 + 3];
    siReg[13] &= ~(1u << 29);
  } else { siReg[13] |= 1u << 29; siReg[14] |= 0x08u << (24 - 8 * ch); }   // COMERR + NOREP
  siReg[13] &= ~1u; siReg[13] |= 0x80000000u;   // TCINT
}

void Machine::siPoll() {
  u32 poll = siReg[12];
  for (int ch = 0; ch < 4; ch++) {
    if (!(poll & (0x80u >> ch))) continue;
    u32 shift = 24 - 8 * ch;
    if (ch != 0) { siReg[14] |= 0x08u << shift; continue; }              // NOREP: nothing plugged in
    u8 resp[16]; int n; padResponse(*this, (siReg[0] >> 16) & 0xFF, resp, n);
    siReg[1] = resp[0] << 24 | resp[1] << 16 | resp[2] << 8 | resp[3];
    siReg[2] = resp[4] << 24 | resp[5] << 16 | resp[6] << 8 | resp[7];
    siReg[14] |= 0x20u << shift;                                          // RDST
  }
  updateInts();
}
