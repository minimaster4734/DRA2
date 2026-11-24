// PowerPC 750CL "Gekko" interpreter (integer, FPU, paired singles).
// Clean-room implementation written from the public PowerPC architecture manuals.
#include "gc.h"
#include <cmath>
#include <cstdlib>

#define RA ((op >> 16) & 31)
#define RB ((op >> 11) & 31)
#define RS ((op >> 21) & 31)
#define RD RS
#define RC (op & 1)
#define SIMM ((s32)(s16)(op & 0xFFFF))
#define UIMM (op & 0xFFFF)
#define FA RA
#define FB RB
#define FC ((op >> 6) & 31)
#define FD RS

enum { XER_SO = 1u << 31, XER_OV = 1u << 30, XER_CA = 1u << 29 };
enum { MSR_EE = 1u << 15, MSR_PR = 1u << 14, MSR_FP = 1u << 13, MSR_ME = 1u << 12, MSR_IP = 1u << 6,
       MSR_IR = 1u << 5, MSR_DR = 1u << 4, MSR_RI = 1u << 1, MSR_LE = 1u << 0 };
enum { SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DEC = 22, SPR_SRR0 = 26, SPR_SRR1 = 27,
       SPR_TBL = 268, SPR_TBU = 269, SPR_GQR0 = 912, SPR_HID2 = 920, SPR_WPAR = 921, SPR_PVR = 287 };

static inline u64 d2u(double d) { u64 u; memcpy(&u, &d, 8); return u; }
static inline double u2d(u64 u) { double d; memcpy(&d, &u, 8); return d; }

static inline u32 mask32(int mb, int me) {
  u32 a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (a & b) : (a | b);
}
static inline u32 rotl(u32 v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline void setCrf(CPU& c, int n, u32 v) {
  int sh = (7 - n) * 4;
  c.cr = (c.cr & ~(0xFu << sh)) | (v << sh);
}
static inline u32 getCrf(CPU& c, int n) { return (c.cr >> ((7 - n) * 4)) & 0xF; }
static inline void cr0(CPU& c, u32 r) {
  u32 v = (s32)r < 0 ? 8 : ((s32)r > 0 ? 4 : 2);
  if (c.xer & XER_SO) v |= 1;
  setCrf(c, 0, v);
}
static inline bool crBit(CPU& c, int bi) { return (c.cr >> (31 - bi)) & 1; }
static inline void setCA(CPU& c, bool ca) { c.xer = (c.xer & ~XER_CA) | (ca ? XER_CA : 0); }
static inline void setOV(CPU& c, bool ov) {
  c.xer = (c.xer & ~XER_OV) | (ov ? XER_OV : 0);
  if (ov) c.xer |= XER_SO;
}

void CPU::exception(u32 vec) {
  { static int k=0; if (icount>2634005000ull && (m->piIntCause & 0x100) && k++<60) m->log("EXC %X pc=%08X cause=%X mask=%X", vec, pc, m->piIntCause, m->piIntMask); }
  if (vec != 0x500 && vec != 0x900 && vec != 0xC00 && GENV("EXCALL")) { static int k2 = 0; if (k2++ < 30) m->log("EXCALL vec=%X pc=%08X srr1=%08X lr=%08X r3=%08X", vec, pc, srr1, lr, gpr[3]); }
  srr0 = pc; srr1 = (msr & 0x87C0FFFF);
  msr &= ~(MSR_EE | MSR_PR | MSR_FP | MSR_IR | MSR_DR | MSR_RI | MSR_LE);
  pc = ((msr & MSR_IP) ? 0xFFF00000u : 0u) + vec;
}

void CPU::checkInterrupts() {
  if (!(msr & MSR_EE)) return;
  if (m->piIntCause & m->piIntMask) { exception(0x500); return; }
  if (decPending) { decPending = false; exception(0x900); }
}

// -------- quantization (GQR) --------
static double dequant(CPU& c, u32 ea, int type, int scale, int* sz) {
  Bus& b = *c.bus;
  double v;
  switch (type) {
    case 4: v = b.fr8(ea); *sz = 1; break;
    case 5: v = b.fr16(ea); *sz = 2; break;
    case 6: v = (s8)b.fr8(ea); *sz = 1; break;
    case 7: v = (s16)b.fr16(ea); *sz = 2; break;
    default: { u32 u = b.fr32(ea); float f; memcpy(&f, &u, 4); v = f; *sz = 4; return v; }
  }
  return v * std::ldexp(1.0, -scale);
}
static void quant(CPU& c, u32 ea, double v, int type, int scale, int* sz) {
  Bus& b = *c.bus;
  if (type == 0 || type == 1 || type == 2 || type == 3) {
    float f = (float)v; u32 u; memcpy(&u, &f, 4); b.fw32(ea, u); *sz = 4; return;
  }
  v = v * std::ldexp(1.0, scale);
  auto clampi = [&](double lo, double hi) { if (v != v) return (s32)0; if (v < lo) v = lo; if (v > hi) v = hi; return (s32)std::trunc(v); };
  switch (type) {
    case 4: b.fw8(ea, (u8)clampi(0, 255)); *sz = 1; break;
    case 5: b.fw16(ea, (u16)clampi(0, 65535)); *sz = 2; break;
    case 6: b.fw8(ea, (u8)(s8)clampi(-128, 127)); *sz = 1; break;
    default: b.fw16(ea, (u16)(s16)clampi(-32768, 32767)); *sz = 2; break;
  }
}
static inline int gqrLdType(u32 g) { return (g >> 16) & 7; }
static inline int gqrLdScale(u32 g) { int s = (g >> 24) & 63; return s >= 32 ? s - 64 : s; }
static inline int gqrStType(u32 g) { return g & 7; }
static inline int gqrStScale(u32 g) { int s = (g >> 8) & 63; return s >= 32 ? s - 64 : s; }

static void psqLoad(CPU& c, u32 op, u32 ea, int i, int w, int fd) {
  u32 g = c.sprs[SPR_GQR0 + i];
  int type = gqrLdType(g), scale = gqrLdScale(g), sz = 0, sz2 = 0;
  double a = dequant(c, ea, type, scale, &sz);
  if (w) { c.ps0[fd] = a; c.ps1[fd] = 1.0; }
  else { double b = dequant(c, ea + sz, type, scale, &sz2); c.ps0[fd] = a; c.ps1[fd] = b; }
}
static void psqStore(CPU& c, u32 op, u32 ea, int i, int w, int fs) {
  u32 g = c.sprs[SPR_GQR0 + i];
  int type = gqrStType(g), scale = gqrStScale(g), sz = 0, sz2 = 0;
  quant(c, ea, c.ps0[fs], type, scale, &sz);
  if (!w) quant(c, ea + sz, c.ps1[fs], type, scale, &sz2);
}

static inline double rsqrt_est(double x) { return 1.0 / std::sqrt(x); }
static inline double f32(double d) { return (double)(float)d; }

static s32 toInt32(double d, bool trunc) {
  if (d != d) return (s32)0x80000000;
  if (!trunc) d = std::nearbyint(d); else d = std::trunc(d);
  if (d >= 2147483648.0) return 0x7FFFFFFF;
  if (d < -2147483648.0) return (s32)0x80000000;
  return (s32)d;
}

static void fcmp(CPU& c, int crf, double a, double b) {
  u32 v;
  if (a != a || b != b) v = 1;
  else if (a < b) v = 8; else if (a > b) v = 4; else v = 2;
  setCrf(c, crf, v);
  c.fpscr = (c.fpscr & ~0x1F000u) | (v << 12);
}

static inline void fpCr1(CPU& c, u32 op) { if (RC) setCrf(c, 1, (c.fpscr >> 28) & 0xF); }

static std::vector<u32> g_pcLogs = []{ std::vector<u32> v; if (const char* e = GENV("PCLOGS")) { char* p = (char*)e; while (*p) { v.push_back((u32)strtoul(p, &p, 16)); if (*p == ',') p++; else break; } } return v; }();
static u64 g_pcStart = GENV("PCSTART") ? strtoull(GENV("PCSTART"),0,10) : 0;
static u32 g_pcLog = GENV("PCLOG") ? (u32)strtoul(GENV("PCLOG"),0,16) : 0;
static u64 g_trA = GENV("TRA") ? strtoull(GENV("TRA"),0,10) : 0, g_trB = GENV("TRB") ? strtoull(GENV("TRB"),0,10) : 0;
u64 g_skipped = 0;
bool CPU::spinHit() { for (int i = 0; i < nspins; i++) if (pc == spins[i].pc) { u32 v = bus->r32(gpr[13] + spins[i].r13off); return (v == 0) == spins[i].whileZero; } return false; }

#include "jit_rt.h"
void CPU::stepJit(int n) {
  Bus& b = *bus; u64 target = icount + n;
  while (icount < target && !halted) {
    if (__builtin_expect(nspins && (pc == spins[0].pc || pc == spins[1].pc) && spinHit(), 0) && (msr & MSR_EE) && !decPending && !(m->piIntCause & m->piIntMask)) {
      u64 t = m->nextEvent(); u64 dl = icount + (u64)dec * 8 + 8; if (dl < t) t = dl;
      if (t > icount + 64) { u64 d8 = (t - icount) / 8; icount += d8 * 8; g_skipped += d8 * 8; tb += d8; dec -= (u32)d8; break; }
    }
    if (__builtin_expect((pc & 0x1FFFFFFF) >= RAM_SIZE || pc >= 0xE0000000u, 0)) { halt("pc outside RAM"); return; }
    JitRT::Entry& e = g_jit->lookup(this, pc);
    u64 room = target - icount; jitCount = 0; jitLimit = room > 512 ? 512 : (u32)room;
    g_jit->be->run(this, b.ram, e.code);
    u32 cnt = jitCount; u64 o8 = icount >> 3; icount += cnt; u64 d = (icount >> 3) - o8;
    if (d) { tb += d; u32 old = dec; dec -= (u32)d; if (d > old) decPending = true; }
    if ((decPending || (m->piIntCause & m->piIntMask))) checkInterrupts();
  }
}
static const bool g_slow = !g_pcLogs.empty() || g_pcLog || g_trA;
void CPU::step(int n) {
  if (g_jit) { stepJit(n); return; }
  Bus& b = *bus;
  if (!g_slow) { for (int k = 0; k < n && !halted; k++) {
    if (__builtin_expect((pc & 0x1FFFFFFF) >= RAM_SIZE, 0)) { halt("pc outside RAM"); return; }
    if (__builtin_expect((pc == spins[0].pc || pc == spins[1].pc) && nspins && spinHit(), 0) && (msr & MSR_EE) && !decPending && !(m->piIntCause & m->piIntMask)) {
      u64 t = m->nextEvent(); u64 dl = icount + (u64)dec * 8 + 8; if (dl < t) t = dl;
      if (t > icount + 64) { u64 d8 = (t - icount) / 8; icount += d8 * 8; g_skipped += d8 * 8; tb += d8; dec -= (u32)d8; break; }
    }
    u32 op = bswap32(*(u32*)(b.ram + (pc & 0x1FFFFFFF))); u32 npc = pc + 4; icount++;
#ifdef OPHIST
    { extern u64 g_oph[64*1024]; g_oph[(op>>26)*1024 + ((op>>26)==31?((op>>1)&1023):(op>>26)==63||(op>>26)==59||(op>>26)==4?((op>>1)&1023):0)]++; }
#endif
    exec(op, npc); pc = npc;
    if ((icount & 7) == 0) { tb += 1; u32 old = dec; dec -= 1; if (old == 0) decPending = true; if (decPending || (m->piIntCause & m->piIntMask)) checkInterrupts(); }
  } return; }
  for (int k = 0; k < n && !halted; k++) {
    pcHist[pcHistPos++ & 63] = pc;
    if (__builtin_expect(!g_pcLogs.empty(), 0) && icount >= g_pcStart) { for (u32 q : g_pcLogs) if (pc == q) { static int kk = 0; if (kk++ < 4000) { auto rw = [&](u32 a) { return (u32)b.ram[a&0x1FFFFFFF]<<24 | b.ram[(a&0x1FFFFFFF)+1]<<16 | b.ram[(a&0x1FFFFFFF)+2]<<8 | b.ram[(a&0x1FFFFFFF)+3]; }; if (GENV("PCBT")) { u32 sp = gpr[1]; char bt[200]; int o = 0; for (int q = 0; q < 8 && sp > 0x80000000u && (sp & 0x1FFFFFFF) < RAM_SIZE; q++) { u32 nsp = rw(sp); u32 ra = rw(nsp + 4); o += snprintf(bt + o, sizeof bt - o, " %08X", ra); sp = nsp; } m->log("PCBT %08X sp=%08X bt:%s", pc, gpr[1], bt); } m->log("PCS %08X lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r29=%08X r30=%08X glob=%08X flag=%08X idx=%08X used=%08X", pc, lr, gpr[3], gpr[4], gpr[5], gpr[6], gpr[7], gpr[29], gpr[30], rw(gpr[13]-0x74a0), rw(gpr[13]-0x74b0), rw(gpr[13]-0x6748), rw(gpr[13]-0x6774)); } } }
    if (__builtin_expect(g_pcLog != 0, 0) && pc == g_pcLog) { for (int rr = 3; rr <= 6; rr++) { u32 p = gpr[rr]; if (inRamPhys(p & 0x1FFFFFFF, 4) && (p >> 28) >= 8) { char t[160]; int i = 0; for (; i < 159; i++) { u8 ch = b.r8(p + i); if (!ch) break; t[i] = ch >= 32 && ch < 127 ? ch : '.'; } t[i] = 0; if (i >= 3) m->log("PCSTR r%d=%08X \"%s\"", rr, p, t); } } }
    if (__builtin_expect(g_pcLog != 0, 0) && pc == g_pcLog) { static int kc=0; if (kc++<400) m->log("PCLOG %08X lr=%08X GQR=%08X %08X %08X %08X %08X %08X %08X %08X r7=%08X", pc, lr, sprs[912],sprs[913],sprs[914],sprs[915],sprs[916],sprs[917],sprs[918],sprs[919], gpr[7]); m->log("R13=%08X glob=%08X", gpr[13], gpr[13]-0x74a0); if (pc == 0x802CF278 && gpr[3] == 9 && (gpr[4] & 0xFFFFFF) >= 0x545000 && (gpr[4] & 0xFFFFFF) < 0x640000) { static int hh = 0; if (hh++ < 4) { u32 h = gpr[30] & 0x1FFFFFFF; m->log("HDR r30=%08X base=%08X stride=%d lr=%08X", gpr[30], gpr[4], gpr[5], lr); for (int q = 0; q < 12; q++) m->log("  +%02X: %08X", q*4, (u32)m->bus.ram[h+q*4]<<24 | m->bus.ram[h+q*4+1]<<16 | m->bus.ram[h+q*4+2]<<8 | m->bus.ram[h+q*4+3]); } } if (pc == 0x802CF278) m->log("GXSETARRAY r3=%d base=%08X stride=%d r30=%08X r29=%08X r31=%08X lr=%08X", gpr[3], gpr[4], gpr[5], gpr[30], gpr[29], gpr[31], lr); }
    if ((pc & 0x1FFFFFFF) >= RAM_SIZE) { halt("pc outside RAM"); return; }
    u32 op = bswap32(*(u32*)(b.ram + (pc & 0x1FFFFFFF)));
    u32 npc = pc + 4;
    if (__builtin_expect(g_trA != 0, 0) && icount >= g_trA && icount < g_trB && (op >> 26) == 18 && (op & 1)) { static int k = 0; if (k++ < 6000) m->log("CALL %08X -> %08X", pc, ((op & 2) ? 0 : pc) + (u32)((s32)(op << 6) >> 6 & ~3)); }
    icount++;
    exec(op, npc);
    pc = npc;
    // time base: 1 tick per 12 cycles; assume ~1.5 cycles/instr
    if ((icount & 7) == 0) {
      tb += 1;
      u32 old = dec; dec -= 1;
      if (old == 0) decPending = true;
      if (decPending || (m->piIntCause & m->piIntMask)) checkInterrupts();
    }
  }
}

#define UNIMPL() do { char buf[96]; snprintf(buf, sizeof buf, "unimplemented op 0x%08X at pc=%08X", op, pc); halt(buf); return; } while (0)

void CPU::exec(u32 op, u32& npc) {
  Bus& b = *bus;
  u32* R = gpr;
  switch (op >> 26) {
    case 3: break; // twi - ignore traps
    case 7: R[RD] = (s32)R[RA] * SIMM; break;
    case 8: { u32 a = R[RA]; u32 imm = SIMM; u64 s = (u64)(~a) + imm + 1; R[RD] = (u32)s; setCA(*this, s >> 32); break; }
    case 10: { u32 a = R[RA], c = UIMM; u32 v = a < c ? 8 : (a > c ? 4 : 2); if (xer & XER_SO) v |= 1; setCrf(*this, (op >> 23) & 7, v); break; }
    case 11: { s32 a = R[RA], c = SIMM; u32 v = a < c ? 8 : (a > c ? 4 : 2); if (xer & XER_SO) v |= 1; setCrf(*this, (op >> 23) & 7, v); break; }
    case 12: { u32 a = R[RA]; u32 imm = SIMM; u64 s = (u64)a + imm; R[RD] = (u32)s; setCA(*this, s >> 32); break; }
    case 13: { u32 a = R[RA]; u32 imm = SIMM; u64 s = (u64)a + imm; R[RD] = (u32)s; setCA(*this, s >> 32); cr0(*this, R[RD]); break; }
    case 14: R[RD] = (RA ? R[RA] : 0) + SIMM; break;
    case 15: R[RD] = (RA ? R[RA] : 0) + (UIMM << 16); break;
    case 16: { // bc
      int bo = RS, bi = RA; u32 tgt = (u32)(s32)(s16)(op & 0xFFFC);
      if (!(bo & 4)) ctr--;
      bool ctrOk = (bo & 4) || ((ctr != 0) ^ ((bo >> 1) & 1));
      bool condOk = (bo & 16) || (crBit(*this, bi) == ((bo >> 3) & 1));
      if (op & 1) lr = pc + 4;
      if (ctrOk && condOk) npc = (op & 2) ? tgt : pc + tgt;
      break; }
    case 17: pc += 4; exception(0xC00); npc = pc; break;
    case 18: { s32 li = (s32)(op & 0x03FFFFFC); if (li & 0x02000000) li |= 0xFC000000;
      if (op & 1) lr = pc + 4; npc = (op & 2) ? (u32)li : pc + li; break; }
    case 19: {
      int xo = (op >> 1) & 0x3FF;
      switch (xo) {
        case 16: case 528: { int bo = RS, bi = RA; if (!(bo & 4) && xo == 16) ctr--;
          bool ctrOk = (xo == 528) || (bo & 4) || ((ctr != 0) ^ ((bo >> 1) & 1));
          bool condOk = (bo & 16) || (crBit(*this, bi) == ((bo >> 3) & 1));
          u32 tgt = (xo == 16 ? lr : ctr) & ~3u; if (op & 1) lr = pc + 4;
          if (ctrOk && condOk) npc = tgt; break; }
        case 50: msr = srr1 & 0x87C0FF73u | (msr & ~0x87C0FF73u); npc = srr0; break; // rfi
        case 150: break; // isync
        case 0: setCrf(*this, (op >> 23) & 7, getCrf(*this, (op >> 18) & 7)); break; // mcrf
        case 257: case 129: case 193: case 225: case 33: case 289: case 417: case 449: {
          int d = RD, a = RA, bb = RB; bool va = crBit(*this, a), vb = crBit(*this, bb), r;
          switch (xo) { case 257: r = va & vb; break; case 129: r = va & !vb; break; case 193: r = va ^ vb; break;
            case 225: r = !(va & vb); break; case 33: r = !(va | vb); break; case 289: r = !(va ^ vb); break;
            case 417: r = va | !vb; break; default: r = va | vb; }
          cr = (cr & ~(1u << (31 - d))) | ((u32)r << (31 - d)); break; }
        default: UNIMPL();
      }
      break; }
    case 20: { u32 m = mask32((op >> 6) & 31, (op >> 1) & 31); u32 r = rotl(R[RS], (op >> 11) & 31);
      R[RA] = (r & m) | (R[RA] & ~m); if (RC) cr0(*this, R[RA]); break; }
    case 21: { u32 m = mask32((op >> 6) & 31, (op >> 1) & 31); R[RA] = rotl(R[RS], (op >> 11) & 31) & m; if (RC) cr0(*this, R[RA]); break; }
    case 23: { u32 m = mask32((op >> 6) & 31, (op >> 1) & 31); R[RA] = rotl(R[RS], R[RB]) & m; if (RC) cr0(*this, R[RA]); break; }
    case 24: R[RA] = R[RS] | UIMM; break;
    case 25: R[RA] = R[RS] | (UIMM << 16); break;
    case 26: R[RA] = R[RS] ^ UIMM; break;
    case 27: R[RA] = R[RS] ^ (UIMM << 16); break;
    case 28: R[RA] = R[RS] & UIMM; cr0(*this, R[RA]); break;
    case 29: R[RA] = R[RS] & (UIMM << 16); cr0(*this, R[RA]); break;
    case 31: {
      int xo = (op >> 1) & 0x3FF; int xo9 = xo & 0x1FF; bool oe = (op >> 10) & 1;
      switch (xo) {
        case 0: { s32 a = R[RA], c = R[RB]; u32 v = a < c ? 8 : (a > c ? 4 : 2); if (xer & XER_SO) v |= 1; setCrf(*this, (op >> 23) & 7, v); break; }
        case 32: { u32 a = R[RA], c = R[RB]; u32 v = a < c ? 8 : (a > c ? 4 : 2); if (xer & XER_SO) v |= 1; setCrf(*this, (op >> 23) & 7, v); break; }
        case 4: break; // tw
        case 19: R[RD] = cr; break;
        case 83: R[RD] = msr; break;
        case 146: msr = R[RS]; break;
        case 144: { u32 crm = (op >> 12) & 0xFF, m = 0; for (int i = 0; i < 8; i++) if (crm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i); cr = (cr & ~m) | (R[RS] & m); break; }
        case 339: { int spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5);
          switch (spr) { case SPR_XER: R[RD] = xer; break; case SPR_LR: R[RD] = lr; break; case SPR_CTR: R[RD] = ctr; break;
            case SPR_DEC: R[RD] = dec; break; case SPR_SRR0: R[RD] = srr0; break; case SPR_SRR1: R[RD] = srr1; break;
            case SPR_PVR: R[RD] = 0x00083214; break; default: R[RD] = sprs[spr & 1023]; }
          break; }
        case 467: { int spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5); u32 v = R[RS];
          switch (spr) { case SPR_XER: xer = v; break; case SPR_LR: lr = v; break; case SPR_CTR: ctr = v; break;
            case SPR_DEC: dec = v; decPending = false; break; case SPR_SRR0: srr0 = v; break; case SPR_SRR1: srr1 = v; break;
            case 923: { sprs[923] = v; if (v & 2) { u32 du = sprs[922]; u32 len = ((du & 0x1F) << 2) | ((v >> 2) & 3); if (!len) len = 128; len *= 32;
                u32 mem = du & 0xFFFFFFE0u & 0x1FFFFFFFu; u32 lcoff = v & 0xFFFFFE0u & 0x3FFFu; if (mem + len <= RAM_SIZE && lcoff + len <= 0x4000) { if ((v >> 4) & 1) memcpy(b.lc + lcoff, b.ram + mem, len); else memcpy(b.ram + mem, b.lc + lcoff, len); } sprs[923] = v & ~3u; } break; }
            default: sprs[spr & 1023] = v; }
          break; }
        case 371: { int tbr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5); R[RD] = tbr == 269 ? (u32)(tb >> 32) : (u32)tb; break; }
        case 595: R[RD] = sr[(op >> 16) & 15]; break;
        case 210: sr[(op >> 16) & 15] = R[RS]; break;
        case 659: R[RD] = sr[R[RB] >> 28]; break;
        case 242: sr[R[RB] >> 28] = R[RS]; break;
        case 982: if (g_jit) g_jit->invalidateRange(((RA ? R[RA] : 0) + R[RB]) & ~31u, 32); break;   // icbi
        case 854: case 54: case 86: case 246: case 278: case 470: case 306: case 566: case 598: break; // sync/eieio/cache hints/tlb
        case 1014: { u32 ea = (RA ? R[RA] : 0) + R[RB]; ea &= ~31u; for (int i = 0; i < 32; i += 4) b.fw32(ea + i, 0); break; } // dcbz
        case 24: { u32 sh = R[RB]; R[RA] = (sh & 0x20) ? 0 : R[RS] << (sh & 31); if (RC) cr0(*this, R[RA]); break; }
        case 536: { u32 sh = R[RB]; R[RA] = (sh & 0x20) ? 0 : R[RS] >> (sh & 31); if (RC) cr0(*this, R[RA]); break; }
        case 792: { u32 sh = R[RB] & 0x3F; s32 s = R[RS]; u32 r; bool ca;
          if (sh >= 32) { r = s >> 31; ca = s < 0; } else { r = s >> sh; ca = s < 0 && sh && (R[RS] & ((1u << sh) - 1)); }
          R[RA] = r; setCA(*this, ca); if (RC) cr0(*this, r); break; }
        case 824: { u32 sh = RB; s32 s = R[RS]; u32 r = sh ? (u32)(s >> sh) : (u32)s; bool ca = s < 0 && sh && (R[RS] & ((1u << sh) - 1)); R[RA] = r; setCA(*this, ca); if (RC) cr0(*this, r); break; }
        case 26: R[RA] = R[RS] ? __builtin_clz(R[RS]) : 32; if (RC) cr0(*this, R[RA]); break;
        case 28: R[RA] = R[RS] & R[RB]; if (RC) cr0(*this, R[RA]); break;
        case 60: R[RA] = R[RS] & ~R[RB]; if (RC) cr0(*this, R[RA]); break;
        case 124: R[RA] = ~(R[RS] | R[RB]); if (RC) cr0(*this, R[RA]); break;
        case 284: R[RA] = ~(R[RS] ^ R[RB]); if (RC) cr0(*this, R[RA]); break;
        case 316: R[RA] = R[RS] ^ R[RB]; if (RC) cr0(*this, R[RA]); break;
        case 412: R[RA] = R[RS] | ~R[RB]; if (RC) cr0(*this, R[RA]); break;
        case 444: R[RA] = R[RS] | R[RB]; if (RC) cr0(*this, R[RA]); break;
        case 476: R[RA] = ~(R[RS] & R[RB]); if (RC) cr0(*this, R[RA]); break;
        case 922: R[RA] = (s32)(s16)R[RS]; if (RC) cr0(*this, R[RA]); break;
        case 954: R[RA] = (s32)(s8)R[RS]; if (RC) cr0(*this, R[RA]); break;
        case 75: R[RD] = (u32)(((s64)(s32)R[RA] * (s64)(s32)R[RB]) >> 32); if (RC) cr0(*this, R[RD]); break;
        case 11: R[RD] = (u32)(((u64)R[RA] * (u64)R[RB]) >> 32); if (RC) cr0(*this, R[RD]); break;
        // loads
        case 23: R[RD] = b.fr32((RA ? R[RA] : 0) + R[RB]); break;
        case 55: { u32 ea = R[RA] + R[RB]; R[RD] = b.fr32(ea); R[RA] = ea; break; }
        case 87: R[RD] = b.fr8((RA ? R[RA] : 0) + R[RB]); break;
        case 119: { u32 ea = R[RA] + R[RB]; R[RD] = b.fr8(ea); R[RA] = ea; break; }
        case 279: R[RD] = b.fr16((RA ? R[RA] : 0) + R[RB]); break;
        case 311: { u32 ea = R[RA] + R[RB]; R[RD] = b.fr16(ea); R[RA] = ea; break; }
        case 343: R[RD] = (s32)(s16)b.fr16((RA ? R[RA] : 0) + R[RB]); break;
        case 375: { u32 ea = R[RA] + R[RB]; R[RD] = (s32)(s16)b.fr16(ea); R[RA] = ea; break; }
        case 534: R[RD] = bswap32(b.fr32((RA ? R[RA] : 0) + R[RB])); break;
        case 790: R[RD] = bswap16(b.fr16((RA ? R[RA] : 0) + R[RB])); break;
        case 20: { u32 ea = (RA ? R[RA] : 0) + R[RB]; R[RD] = b.fr32(ea); reserve = ea; reserveValid = true; break; }
        // stores
        case 151: b.fw32((RA ? R[RA] : 0) + R[RB], R[RS]); break;
        case 183: { u32 ea = R[RA] + R[RB]; b.fw32(ea, R[RS]); R[RA] = ea; break; }
        case 215: b.fw8((RA ? R[RA] : 0) + R[RB], R[RS]); break;
        case 247: { u32 ea = R[RA] + R[RB]; b.fw8(ea, R[RS]); R[RA] = ea; break; }
        case 407: b.fw16((RA ? R[RA] : 0) + R[RB], R[RS]); break;
        case 439: { u32 ea = R[RA] + R[RB]; b.fw16(ea, R[RS]); R[RA] = ea; break; }
        case 662: b.fw32((RA ? R[RA] : 0) + R[RB], bswap32(R[RS])); break;
        case 918: b.fw16((RA ? R[RA] : 0) + R[RB], bswap16(R[RS])); break;
        case 150: { u32 ea = (RA ? R[RA] : 0) + R[RB]; if (reserveValid && reserve == ea) { b.fw32(ea, R[RS]); setCrf(*this, 0, 2 | ((xer & XER_SO) ? 1 : 0)); }
          else setCrf(*this, 0, (xer & XER_SO) ? 1 : 0); reserveValid = false; break; }
        case 535: { u32 u = b.fr32((RA ? R[RA] : 0) + R[RB]); float f; memcpy(&f, &u, 4); ps0[FD] = f; ps1[FD] = f; break; }
        case 567: { u32 ea = R[RA] + R[RB]; u32 u = b.fr32(ea); float f; memcpy(&f, &u, 4); ps0[FD] = f; ps1[FD] = f; R[RA] = ea; break; }
        case 599: ps0[FD] = u2d(b.r64((RA ? R[RA] : 0) + R[RB])); break;
        case 631: { u32 ea = R[RA] + R[RB]; ps0[FD] = u2d(b.r64(ea)); R[RA] = ea; break; }
        case 663: { float f = (float)ps0[FD]; u32 u; memcpy(&u, &f, 4); b.fw32((RA ? R[RA] : 0) + R[RB], u); break; }
        case 695: { u32 ea = R[RA] + R[RB]; float f = (float)ps0[FD]; u32 u; memcpy(&u, &f, 4); b.fw32(ea, u); R[RA] = ea; break; }
        case 727: b.w64((RA ? R[RA] : 0) + R[RB], d2u(ps0[FD])); break;
        case 759: { u32 ea = R[RA] + R[RB]; b.w64(ea, d2u(ps0[FD])); R[RA] = ea; break; }
        case 983: b.fw32((RA ? R[RA] : 0) + R[RB], (u32)d2u(ps0[FD])); break;
        case 533: case 597: case 661: case 725: UNIMPL();
        default: {
          // arithmetic with OE (xo9 groups)
          u32 a = R[RA], c = R[RB]; u32 r; bool ca = false, ov = false; bool handled = true;
          switch (xo9) {
            case 266: r = a + c; ov = (~(a ^ c) & (a ^ r)) >> 31; break;
            case 40: r = c - a; ov = ((c ^ a) & (c ^ r)) >> 31; break;
            case 10: { u64 s = (u64)a + c; r = (u32)s; ca = s >> 32; ov = (~(a ^ c) & (a ^ r)) >> 31; setCA(*this, ca); break; }
            case 8: { u64 s = (u64)(~a) + c + 1; r = (u32)s; ca = s >> 32; ov = ((c ^ a) & (c ^ r)) >> 31; setCA(*this, ca); break; }
            case 138: { u64 s = (u64)a + c + ((xer & XER_CA) ? 1 : 0); r = (u32)s; setCA(*this, s >> 32); ov = (~(a ^ c) & (a ^ r)) >> 31; break; }
            case 136: { u64 s = (u64)(~a) + c + ((xer & XER_CA) ? 1 : 0); r = (u32)s; setCA(*this, s >> 32); ov = ((c ^ a) & (c ^ r)) >> 31; break; }
            case 202: { u64 s = (u64)a + ((xer & XER_CA) ? 1 : 0); r = (u32)s; setCA(*this, s >> 32); ov = (~(a ^ 0) & (a ^ r)) >> 31 & 0; break; }
            case 234: { u64 s = (u64)a + 0xFFFFFFFFull + ((xer & XER_CA) ? 1 : 0); r = (u32)s; setCA(*this, s >> 32); break; }
            case 200: { u64 s = (u64)(~a) + ((xer & XER_CA) ? 1 : 0); r = (u32)s; setCA(*this, s >> 32); break; }
            case 232: { u64 s = (u64)(~a) + 0xFFFFFFFFull + ((xer & XER_CA) ? 1 : 0); r = (u32)s; setCA(*this, s >> 32); break; }
            case 104: r = -a; ov = a == 0x80000000u; break;
            case 235: { s64 p = (s64)(s32)a * (s64)(s32)c; r = (u32)p; ov = p != (s32)r; break; }
            case 459: { r = c ? a / c : 0; ov = c == 0; break; }
            case 491: { s32 sa = a, sc = c; if (sc == 0 || (sa == INT32_MIN && sc == -1)) { r = (sa < 0) ? 0xFFFFFFFF : 0; ov = true; } else r = sa / sc; break; }
            default: handled = false; r = 0;
          }
          if (!handled) UNIMPL();
          R[RD] = r; if (oe) setOV(*this, ov); if (RC) cr0(*this, r);
        }
      }
      break; }
    // load / store D-form
    case 32: R[RD] = b.fr32((RA ? R[RA] : 0) + SIMM); break;
    case 33: { u32 ea = R[RA] + SIMM; R[RD] = b.fr32(ea); R[RA] = ea; break; }
    case 34: R[RD] = b.fr8((RA ? R[RA] : 0) + SIMM); break;
    case 35: { u32 ea = R[RA] + SIMM; R[RD] = b.fr8(ea); R[RA] = ea; break; }
    case 36: b.fw32((RA ? R[RA] : 0) + SIMM, R[RS]); break;
    case 37: { u32 ea = R[RA] + SIMM; b.fw32(ea, R[RS]); R[RA] = ea; break; }
    case 38: b.fw8((RA ? R[RA] : 0) + SIMM, R[RS]); break;
    case 39: { u32 ea = R[RA] + SIMM; b.fw8(ea, R[RS]); R[RA] = ea; break; }
    case 40: R[RD] = b.fr16((RA ? R[RA] : 0) + SIMM); break;
    case 41: { u32 ea = R[RA] + SIMM; R[RD] = b.fr16(ea); R[RA] = ea; break; }
    case 42: R[RD] = (s32)(s16)b.fr16((RA ? R[RA] : 0) + SIMM); break;
    case 43: { u32 ea = R[RA] + SIMM; R[RD] = (s32)(s16)b.fr16(ea); R[RA] = ea; break; }
    case 44: b.fw16((RA ? R[RA] : 0) + SIMM, R[RS]); break;
    case 45: { u32 ea = R[RA] + SIMM; b.fw16(ea, R[RS]); R[RA] = ea; break; }
    case 46: { u32 ea = (RA ? R[RA] : 0) + SIMM; for (int r = RD; r < 32; r++, ea += 4) R[r] = b.fr32(ea); break; }
    case 47: { u32 ea = (RA ? R[RA] : 0) + SIMM; for (int r = RS; r < 32; r++, ea += 4) b.fw32(ea, R[r]); break; }
    case 48: { u32 u = b.fr32((RA ? R[RA] : 0) + SIMM); float f; memcpy(&f, &u, 4); ps0[FD] = f; ps1[FD] = f; break; }
    case 49: { u32 ea = R[RA] + SIMM; u32 u = b.fr32(ea); float f; memcpy(&f, &u, 4); ps0[FD] = f; ps1[FD] = f; R[RA] = ea; break; }
    case 50: ps0[FD] = u2d(b.r64((RA ? R[RA] : 0) + SIMM)); break;
    case 51: { u32 ea = R[RA] + SIMM; ps0[FD] = u2d(b.r64(ea)); R[RA] = ea; break; }
    case 52: { float f = (float)ps0[FD]; u32 u; memcpy(&u, &f, 4); b.fw32((RA ? R[RA] : 0) + SIMM, u); break; }
    case 53: { u32 ea = R[RA] + SIMM; float f = (float)ps0[FD]; u32 u; memcpy(&u, &f, 4); b.fw32(ea, u); R[RA] = ea; break; }
    case 54: b.w64((RA ? R[RA] : 0) + SIMM, d2u(ps0[FD])); break;
    case 55: { u32 ea = R[RA] + SIMM; b.w64(ea, d2u(ps0[FD])); R[RA] = ea; break; }
    case 56: psqLoad(*this, op, (RA ? R[RA] : 0) + ((s32)(op & 0xFFF) << 20 >> 20), (op >> 12) & 7, (op >> 15) & 1, FD); break;
    case 57: { u32 ea = R[RA] + ((s32)(op & 0xFFF) << 20 >> 20); psqLoad(*this, op, ea, (op >> 12) & 7, (op >> 15) & 1, FD); R[RA] = ea; break; }
    case 60: psqStore(*this, op, (RA ? R[RA] : 0) + ((s32)(op & 0xFFF) << 20 >> 20), (op >> 12) & 7, (op >> 15) & 1, FD); break;
    case 61: { u32 ea = R[RA] + ((s32)(op & 0xFFF) << 20 >> 20); psqStore(*this, op, ea, (op >> 12) & 7, (op >> 15) & 1, FD); R[RA] = ea; break; }
    case 4: execPS(op); break;
    case 59: case 63: execFP(op); break;
    default: UNIMPL();
  }
}

void CPU::execPS(u32 op) {
  int xo10 = (op >> 1) & 0x3FF, xo5 = (op >> 1) & 0x1F;
  int d = FD, a = FA, bb = FB, c = FC;
  { int xo6 = xo10 & 0x3F;
    if (xo6 == 6 || xo6 == 38) { u32 ea = (RA ? gpr[RA] : 0) + gpr[RB]; psqLoad(*this, op, ea, (op >> 7) & 7, (op >> 10) & 1, FD); if (xo6 == 38) gpr[RA] = ea; return; }
    if (xo6 == 7 || xo6 == 39) { u32 ea = (RA ? gpr[RA] : 0) + gpr[RB]; psqStore(*this, op, ea, (op >> 7) & 7, (op >> 10) & 1, FD); if (xo6 == 39) gpr[RA] = ea; return; } }
  switch (xo10) {
    case 0: fcmp(*this, (op >> 23) & 7, ps0[a], ps0[bb]); return;
    case 32: fcmp(*this, (op >> 23) & 7, ps0[a], ps0[bb]); return;
    case 64: fcmp(*this, (op >> 23) & 7, ps1[a], ps1[bb]); return;
    case 96: fcmp(*this, (op >> 23) & 7, ps1[a], ps1[bb]); return;
    case 40: { double x = -ps0[bb], y = -ps1[bb]; ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 72: { double x = ps0[bb], y = ps1[bb]; ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 136: { double x = -std::fabs(ps0[bb]), y = -std::fabs(ps1[bb]); ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 264: { double x = std::fabs(ps0[bb]), y = std::fabs(ps1[bb]); ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 528: { double x = ps0[a], y = ps0[bb]; ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 560: { double x = ps0[a], y = ps1[bb]; ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 592: { double x = ps1[a], y = ps0[bb]; ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 624: { double x = ps1[a], y = ps1[bb]; ps0[d] = x; ps1[d] = y; fpCr1(*this, op); return; }
    case 6: case 38: { u32 ea = (RA ? gpr[RA] : 0) + gpr[RB]; psqLoad(*this, op, ea, (op >> 7) & 7, (op >> 10) & 1, FD); if (xo10 == 38) gpr[RA] = ea; return; }
    case 7: case 39: { u32 ea = (RA ? gpr[RA] : 0) + gpr[RB]; psqStore(*this, op, ea, (op >> 7) & 7, (op >> 10) & 1, FD); if (xo10 == 39) gpr[RA] = ea; return; }
    case 1014: { u32 ea = ((RA ? gpr[RA] : 0) + gpr[RB]) & ~31u; for (int i = 0; i < 32; i += 4) bus->w32(ea + i, 0); return; }
    default: break;
  }
  double x0, x1;
  switch (xo5) {
    case 18: x0 = ps0[a] / ps0[bb]; x1 = ps1[a] / ps1[bb]; break;
    case 20: x0 = ps0[a] - ps0[bb]; x1 = ps1[a] - ps1[bb]; break;
    case 21: x0 = ps0[a] + ps0[bb]; x1 = ps1[a] + ps1[bb]; break;
    case 23: x0 = ps0[a] >= 0 ? ps0[c] : ps0[bb]; x1 = ps1[a] >= 0 ? ps1[c] : ps1[bb]; break;
    case 24: x0 = 1.0 / ps0[bb]; x1 = 1.0 / ps1[bb]; break;
    case 25: x0 = ps0[a] * ps0[c]; x1 = ps1[a] * ps1[c]; break;
    case 26: x0 = rsqrt_est(ps0[bb]); x1 = rsqrt_est(ps1[bb]); break;
    case 28: x0 = ps0[a] * ps0[c] - ps0[bb]; x1 = ps1[a] * ps1[c] - ps1[bb]; break;
    case 29: x0 = ps0[a] * ps0[c] + ps0[bb]; x1 = ps1[a] * ps1[c] + ps1[bb]; break;
    case 30: x0 = -(ps0[a] * ps0[c] - ps0[bb]); x1 = -(ps1[a] * ps1[c] - ps1[bb]); break;
    case 31: x0 = -(ps0[a] * ps0[c] + ps0[bb]); x1 = -(ps1[a] * ps1[c] + ps1[bb]); break;
    case 10: x0 = ps0[a] * 1.0 + ps1[bb]; x1 = ps1[c]; break;   // ps_sum0
    case 11: x0 = ps0[c]; x1 = ps0[a] + ps1[bb]; break;         // ps_sum1
    case 12: x0 = ps0[a] * ps0[c]; x1 = ps1[a] * ps0[c]; break; // ps_muls0
    case 13: x0 = ps0[a] * ps1[c]; x1 = ps1[a] * ps1[c]; break; // ps_muls1
    case 14: x0 = ps0[a] * ps0[c] + ps0[bb]; x1 = ps1[a] * ps0[c] + ps1[bb]; break; // ps_madds0
    case 15: x0 = ps0[a] * ps1[c] + ps0[bb]; x1 = ps1[a] * ps1[c] + ps1[bb]; break; // ps_madds1
    default: { char buf[96]; snprintf(buf, sizeof buf, "unimplemented PS op 0x%08X at pc=%08X", op, pc); halt(buf); return; }
  }
  ps0[d] = f32(x0); ps1[d] = f32(x1); fpCr1(*this, op);
}

void CPU::execFP(u32 op) {
  int d = FD, a = FA, bb = FB, c = FC;
  bool single = (op >> 26) == 59;
  int xo10 = (op >> 1) & 0x3FF, xo5 = (op >> 1) & 0x1F;
  if (!single) {
    switch (xo10) {
      case 0: case 32: fcmp(*this, (op >> 23) & 7, ps0[a], ps0[bb]); return;
      case 12: ps0[d] = f32(ps0[bb]); fpCr1(*this, op); return;
      case 14: { s32 v = toInt32(ps0[bb], false); ps0[d] = u2d(0xFFF8000000000000ull | (u32)v); fpCr1(*this, op); return; }
      case 15: { s32 v = toInt32(ps0[bb], true); ps0[d] = u2d(0xFFF8000000000000ull | (u32)v); fpCr1(*this, op); return; }
      case 40: ps0[d] = -ps0[bb]; fpCr1(*this, op); return;
      case 72: ps0[d] = ps0[bb]; fpCr1(*this, op); return;
      case 136: ps0[d] = -std::fabs(ps0[bb]); fpCr1(*this, op); return;
      case 264: ps0[d] = std::fabs(ps0[bb]); fpCr1(*this, op); return;
      case 583: ps0[d] = u2d(0xFFF8000000000000ull | fpscr); return;
      case 711: { u32 fm = (op >> 17) & 0xFF, m = 0; for (int i = 0; i < 8; i++) if (fm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i);
        fpscr = (fpscr & ~m) | ((u32)d2u(ps0[bb]) & m); return; }
      case 38: case 70: case 134: case 64: return; // mtfsb1/0, mtfsfi, mcrfs
      default: break;
    }
  }
  double x;
  switch (xo5) {
    case 18: x = ps0[a] / ps0[bb]; break;
    case 20: x = ps0[a] - ps0[bb]; break;
    case 21: x = ps0[a] + ps0[bb]; break;
    case 23: if (single) { char buf[64]; snprintf(buf, sizeof buf, "bad op %08X", op); halt(buf); return; } x = ps0[a] >= 0 ? ps0[c] : ps0[bb]; break;
    case 24: x = 1.0 / ps0[bb]; break;
    case 25: x = ps0[a] * ps0[c]; break;
    case 26: if (single) { halt("bad op"); return; } x = rsqrt_est(ps0[bb]); break;
    case 28: x = ps0[a] * ps0[c] - ps0[bb]; break;
    case 29: x = ps0[a] * ps0[c] + ps0[bb]; break;
    case 30: x = -(ps0[a] * ps0[c] - ps0[bb]); break;
    case 31: x = -(ps0[a] * ps0[c] + ps0[bb]); break;
    default: { char buf[96]; snprintf(buf, sizeof buf, "unimplemented FP op 0x%08X at pc=%08X", op, pc); halt(buf); return; }
  }
  if (single) { x = f32(x); ps0[d] = x; ps1[d] = x; } else ps0[d] = x;
  fpCr1(*this, op);
}
