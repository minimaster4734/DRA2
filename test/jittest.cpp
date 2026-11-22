// Differential test: A32 code emitted by the JIT (run under the Unicorn ARM emulator) vs. the interpreter.
#include "../core/gc.h"
#include "../core/jit.h"
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>

static const u64 CTX_ADDR = 0x10000000, RAM_ADDR = 0x20000000, CODE_ADDR = 0x30000000, STACK_ADDR = 0x40000000, MAGIC = 0x70000000, RET_ADDR = 0x71000000;
static uc_engine* uc; static CPU* cj; static Bus* busJ;

static void hook(uc_engine* u, u64 addr, u32, void*) {
  u32 r0, r1, r2, lr; uc_reg_read(u, UC_ARM_REG_R0, &r0); uc_reg_read(u, UC_ARM_REG_R1, &r1); uc_reg_read(u, UC_ARM_REG_R2, &r2); uc_reg_read(u, UC_ARM_REG_LR, &lr);
  u32 ret = 0; int f = (addr - MAGIC) / 16;
  switch (f) {
    case 0: { u32 npc = r2 + 4; cj->pc = r2; cj->exec(r1, npc); ret = npc; break; }
    case 1: ret = busJ->r8(r1); break; case 2: ret = busJ->r16(r1); break; case 3: ret = busJ->r32(r1); break;
    case 4: busJ->w8(r1, r2); break; case 5: busJ->w16(r1, r2); break; case 6: busJ->w32(r1, r2); break;
  }
  uc_reg_write(u, UC_ARM_REG_R0, &ret); uc_reg_write(u, UC_ARM_REG_PC, &lr);
}

static u32 bswap(u32 v) { return __builtin_bswap32(v); }
static std::mt19937 rng(getenv("SEED") ? atoi(getenv("SEED")) : 12345);
static u32 rnd(u32 n) { return rng() % n; }

// instruction generators ---------------------------------------------------------------------------------------
static u32 fpWord() {   // biased 32-bit pattern: exercises zero / denormal / inf-nan / range boundaries as single and as double hi word
  u32 w = rng();
  switch (rnd(14)) { case 0: return 0; case 1: return 0x80000000u; case 2: return w & 0x807FFFFFu; case 3: return w | 0x7F800000u;
    case 4: return w & 0x800FFFFFu; case 5: return w | 0x7FF00000u; case 6: return (w & 0x800FFFFFu) | ((896 + 1 + rnd(252)) << 20);
    case 7: return (w & 0x800FFFFFu) | ((u32[]){896, 897, 1149, 1150, 1151}[rnd(5)] << 20); case 8: return (w & 0x807FFFFFu) | ((1 + rnd(254)) << 23);
    case 9: return 0x10000000u; default: return w; }
}
static u32 fpLo() { switch (rnd(5)) { case 0: return 0; case 1: return 0x10000000u | (rnd(2) << 29); case 2: return 0x10000001u; case 3: return rng() & 0xE0000000u; default: return rng(); } }
static u32 gprData() { return rnd(20); }        // r0-r19: data registers
static u32 gprPtr() { return 20 + rnd(8); }     // r20-r27: pointers into RAM
static u32 gprOff() { return 28 + rnd(4); }     // r28-r31: small offsets
static u32 gen(bool allowBranch) {
  u32 d = gprData(), a = gprData(), b = gprData(), rc = rnd(2);
  u32 simm = rnd(4) == 0 ? rnd(65536) : rnd(64), sh = rnd(32), mb = rnd(32), me = rnd(32);
  if (rnd(3) == 0) simm = (u32)(-(int)rnd(64)) & 0xFFFF;
  int k = rnd(allowBranch ? 70 : 62); if (rnd(7) == 0 || getenv("FPK")) k = 70 + rnd(4);
  switch (k) {
    case 0: return 14u << 26 | d << 21 | (rnd(3) ? a : 0) << 16 | simm;
    case 1: return 15u << 26 | d << 21 | (rnd(3) ? a : 0) << 16 | simm;
    case 2: return 24u << 26 | d << 21 | a << 16 | simm;
    case 3: return 25u << 26 | d << 21 | a << 16 | simm;
    case 4: return 26u << 26 | d << 21 | a << 16 | simm;
    case 5: return 27u << 26 | d << 21 | a << 16 | simm;
    case 6: return 28u << 26 | d << 21 | a << 16 | simm;
    case 7: return 29u << 26 | d << 21 | a << 16 | simm;
    case 8: return 10u << 26 | rnd(8) << 23 | a << 16 | simm;
    case 9: return 11u << 26 | rnd(8) << 23 | a << 16 | simm;
    case 10: return 21u << 26 | d << 21 | a << 16 | sh << 11 | mb << 6 | me << 1 | rc;
    case 11: return 20u << 26 | d << 21 | a << 16 | sh << 11 | mb << 6 | me << 1 | rc;
    case 12: return 23u << 26 | d << 21 | a << 16 | b << 11 | mb << 6 | me << 1 | rc;   // rlwnm (helper)
    case 13: case 14: case 15: case 16: { u32 xo[] = {28, 60, 316, 444, 124, 24, 536, 26, 922, 954, 266, 40, 104, 235, 75, 11, 412, 476, 284, 792, 824, 0, 32}; u32 x = xo[rnd(23)];
      bool cmp = x == 0 || x == 32; return 31u << 26 | (cmp ? rnd(8) << 23 : d << 21) | a << 16 | b << 11 | x << 1 | (cmp ? 0 : rc); }
    case 17: case 18: case 19: case 20: case 21: case 22: { u32 x[] = {266, 40, 28, 444, 316, 24}; return 31u << 26 | d << 21 | a << 16 | b << 11 | (x[rnd(6)]) << 1 | rc; }
    case 23: case 24: case 25: return 12u << 26 | d << 21 | a << 16 | simm;      // addic (helper)
    case 26: return 7u << 26 | d << 21 | a << 16 | simm;                          // mulli (helper)
    case 27: return 8u << 26 | d << 21 | a << 16 | simm;                          // subfic (helper)
    case 28: return 31u << 26 | d << 21 | a << 16 | b << 11 | 491u << 1 | rc;      // divw (helper)
    case 29: return 31u << 26 | d << 21 | a << 16 | b << 11 | 459u << 1 | rc;      // divwu
    case 30: return 31u << 26 | d << 21 | a << 16 | sh << 11 | 824u << 1 | rc;      // srawi
    case 31: return 31u << 26 | d << 21 | a << 16 | b << 11 | 792u << 1 | rc;      // sraw
    case 32: return 31u << 26 | d << 21 | 19u << 1;                                // mfcr
    case 33: return 31u << 26 | d << 21 | (0xFF) << 12 | 144u << 1;                // mtcrf
    case 34: { u32 spr = (u32[]){8, 9, 1}[rnd(3)]; return 31u << 26 | d << 21 | ((spr & 31) << 16) | ((spr >> 5) << 11) | (rnd(2) ? 339u : 467u) << 1; }
    case 35: return 19u << 26 | rnd(8) << 23 | rnd(8) << 18;                       // mcrf (helper)
    case 36: return 19u << 26 | rnd(32) << 21 | rnd(32) << 16 | rnd(32) << 11 | (u32[]){257, 129, 193, 225, 33, 289, 417, 449}[rnd(8)] << 1;  // crand..
    // loads/stores (D form)
    case 37: case 38: case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48: {
      u32 opc[] = {32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45}; u32 o = opc[rnd(14)]; u32 ra = gprPtr(); u32 dd = rnd(0x200);
      bool upd = (o == 33 || o == 35 || o == 41 || o == 43 || o == 37 || o == 39 || o == 45); u32 rd = upd ? (u32)(rnd(20)) : d;
      if (!upd && rnd(5) == 0) ra = 0;
      if (ra == 0) { dd = 0x1000 + rnd(0x1000); }
      return o << 26 | rd << 21 | ra << 16 | dd; }
    case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 60: case 61: {
      u32 xo[] = {23, 87, 279, 343, 151, 215, 407, 55, 119, 311, 375, 183, 247, 439}; u32 x = xo[rnd(14)]; bool upd = (x == 55 || x == 119 || x == 311 || x == 375 || x == 183 || x == 247 || x == 439);
      return 31u << 26 | d << 21 | (gprPtr()) << 16 | gprOff() << 11 | x << 1; (void)upd; }
    case 70: case 71: { u32 o = (u32[]){48, 49, 50, 51}[rnd(4)]; bool upd = o & 1; u32 ra = gprPtr(); u32 dd = rnd(0x200); if (!upd && rnd(5) == 0) { ra = 0; dd = 0x1000 + rnd(0x1000); } return o << 26 | rnd(32) << 21 | ra << 16 | dd; }
    case 72: case 73: { u32 o = (u32[]){52, 53, 54, 55}[rnd(4)]; bool upd = o & 1; u32 ra = gprPtr(); u32 dd = rnd(0x200); if (!upd && rnd(5) == 0) { ra = 0; dd = 0x1000 + rnd(0x1000); } return o << 26 | rnd(32) << 21 | ra << 16 | dd; }
    // branches
    case 62: case 63: case 64: { u32 bo = (u32[]){0, 2, 4, 8, 10, 12, 16, 18, 20}[rnd(9)]; s32 disp = (rnd(8) - 3) * 4; if (disp == 0) disp = 8; return 16u << 26 | bo << 21 | rnd(32) << 16 | ((u32)disp & 0xFFFC) | (rnd(4) == 0); }
    case 65: return 18u << 26 | (((rnd(32) - 8) * 4) & 0x03FFFFFC) | (rnd(2));
    case 66: case 67: { u32 bo = (u32[]){4, 12, 20, 16, 8}[rnd(5)]; return 19u << 26 | bo << 21 | rnd(32) << 16 | (rnd(2) ? 16u : 528u) << 1 | rnd(2); }
    default: return 24u << 26 | d << 21 | d << 16 | simm;
  }
}

static void randState(CPU& c) {
  for (int i = 0; i < 32; i++) {
    u32 v = rng();
    switch (rnd(6)) { case 0: v = 0; break; case 1: v = rnd(100); break; case 2: v = 0xFFFFFFFFu - rnd(100); break; case 3: v = 0x80000000u | rnd(32); break; default: break; }
    c.gpr[i] = v;
  }
  for (int i = 20; i < 28; i++) { u32 k = rnd(10); c.gpr[i] = k == 0 ? 0xE0000100u + rnd(0x2000) * 4 : k == 1 ? 0xC0200000u + rnd(0x1000) * 4 : 0x80200000u + rnd(0x4000) * 4; }
  for (int i = 28; i < 32; i++) c.gpr[i] = rnd(0x400) * 4;
  c.cr = rng(); c.xer = (rng() & 0xE0000000u); c.lr = 0x80010000u + rnd(16) * 4; c.ctr = rnd(3) ? 1 + rnd(4) : 0x80010000u + rnd(16) * 4;
}

int main(int argc, char** argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 2000; int K = getenv("K") ? atoi(getenv("K")) : 14;
  size_t cpuSz = (sizeof(CPU) + 4095) & ~4095ul;
  void* mem = aligned_alloc(4096, cpuSz); memset(mem, 0, cpuSz); cj = new (mem) CPU();
  Bus bA; busJ = new Bus(); CPU* ca = new CPU();
  free(busJ->ram); busJ->ram = (u8*)aligned_alloc(4096, RAM_SIZE + 4096); memset(busJ->ram, 0, RAM_SIZE + 4096);
  cj->bus = busJ; ca->bus = &bA;
  uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
  uc_mem_map_ptr(uc, CTX_ADDR, cpuSz, UC_PROT_ALL, cj);
  { u32* ft = (u32*)aligned_alloc(4096, 65536 * 8); for (u32 i = 0; i < 65536 * 2; i++) ft[i] = 1; uc_mem_map_ptr(uc, 0x50000000, 65536 * 8, UC_PROT_ALL, ft); cj->jitFast = 0x50000000; }
  uc_mem_map_ptr(uc, RAM_ADDR, (RAM_SIZE + 4095) & ~4095u, UC_PROT_ALL, busJ->ram);
  uc_mem_map(uc, CODE_ADDR, 0x100000, UC_PROT_ALL); uc_mem_map(uc, STACK_ADDR, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, MAGIC, 0x1000, UC_PROT_ALL); uc_mem_map(uc, RET_ADDR, 0x1000, UC_PROT_ALL);
  static bool trace = getenv("TRACE") != nullptr; if (trace) { uc_hook th; uc_hook_add(uc, &th, UC_HOOK_CODE, (void*)+[](uc_engine* u, u64 a, u32 sz, void*) { u32 w; uc_mem_read(u, a, &w, 4); printf("  pc=%08llX %08X\n", (unsigned long long)a, w); }, nullptr, CODE_ADDR, CODE_ADDR + 0xFFFFF); }
  uc_hook hh; uc_hook_add(uc, &hh, UC_HOOK_CODE, (void*)hook, nullptr, MAGIC, MAGIC + 0xFF);
  // fill the helper stubs with a nop so the hook fires
  for (int i = 0; i < 16; i++) { u32 nop = 0xE320F000; uc_mem_write(uc, MAGIC + i * 16, &nop, 4); }
  JitHelpers hp{(u32)MAGIC, (u32)MAGIC + 16, (u32)MAGIC + 32, (u32)MAGIC + 48, (u32)MAGIC + 64, (u32)MAGIC + 80, (u32)MAGIC + 96};

  int fails = 0, totalInstr = 0;
  for (int it = 0; it < iters && fails < 8; it++) {
    memset(busJ->ram, 0, 0x400000); memset(bA.ram, 0, 0x400000);
    for (int i = 0; i < 0x4000; i++) { u8 v = rng(); busJ->ram[0x200000 + i] = v; bA.ram[0x200000 + i] = v; }
    for (int i = 0; i < 0x4000; i += 8) { u32 hi = fpWord(), lo = rnd(2) ? fpLo() : fpWord(); u32 be[2] = {bswap(hi), bswap(lo)}; memcpy(busJ->ram + 0x200000 + i, be, 8); memcpy(bA.ram + 0x200000 + i, be, 8); }
    memset(busJ->lc, 0x5A, LC_SIZE); memset(bA.lc, 0x5A, LC_SIZE);
    std::vector<u32> words(K);
    for (int i = 0; i < K; i++) { words[i] = gen(i >= 2 || rnd(3) == 0); }
    for (int i = 0; i < K; i++) { u32 be = bswap(words[i]); memcpy(busJ->ram + 0x10000 + 4 * i, &be, 4); memcpy(bA.ram + 0x10000 + 4 * i, &be, 4); }
    CPU init; randState(init);
    memcpy(cj->gpr, init.gpr, sizeof init.gpr); cj->cr = init.cr; cj->xer = init.xer; cj->lr = init.lr; cj->ctr = init.ctr; cj->pc = 0x80010000;
    memcpy(ca->gpr, init.gpr, sizeof init.gpr); ca->cr = init.cr; ca->xer = init.xer; ca->lr = init.lr; ca->ctr = init.ctr; ca->pc = 0x80010000;
    cj->msr = ca->msr = 0x2000;
    for (int i = 0; i < 32; i++) { u64 v0 = ((u64)fpWord() << 32) | fpLo(), v1 = ((u64)fpWord() << 32) | fpLo(); memcpy(&cj->ps0[i], &v0, 8); memcpy(&ca->ps0[i], &v0, 8); memcpy(&cj->ps1[i], &v1, 8); memcpy(&ca->ps1[i], &v1, 8); }
    std::vector<u32> code; int n = jitTranslate(code, hp, 0x80010000, words.data(), K);
    uc_mem_write(uc, CODE_ADDR, code.data(), code.size() * 4); uc_ctl_flush_tb(uc);
    u32 r0 = CTX_ADDR, r1 = RAM_ADDR, sp = STACK_ADDR + 0x8000, lr = RET_ADDR; 
    uc_reg_write(uc, UC_ARM_REG_R0, &r0); uc_reg_write(uc, UC_ARM_REG_R1, &r1); uc_reg_write(uc, UC_ARM_REG_SP, &sp); uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    cj->jitCount = 0; cj->jitLimit = 0;
    uc_err e = uc_emu_start(uc, CODE_ADDR, RET_ADDR, 0, 200000);
    if (e) { printf("iter %d: unicorn error %s\n", it, uc_strerror(e)); fails++; continue; }
    if (getenv("DBG")) { u32 pcv; uc_reg_read(uc, UC_ARM_REG_PC, &pcv); printf("stopped pc=%08X code words=%zu first=%08X\n", pcv, code.size(), code[0]); }
    int cnt = cj->jitCount; totalInstr += cnt;
    for (int i = 0; i < cnt; i++) { u32 npc = ca->pc + 4; u32 op = bswap(*(u32*)(bA.ram + (ca->pc & 0x1FFFFFFF))); ca->exec(op, npc); ca->pc = npc; }
    bool bad = false; char why[160] = "";
    for (int i = 0; i < 32; i++) if (cj->gpr[i] != ca->gpr[i]) { bad = true; snprintf(why, sizeof why, "r%d jit=%08X ref=%08X", i, cj->gpr[i], ca->gpr[i]); break; }
    for (int i = 0; i < 32 && !bad; i++) if (memcmp(&cj->ps0[i], &ca->ps0[i], 8) || memcmp(&cj->ps1[i], &ca->ps1[i], 8)) { bad = true; u64 a0, a1, b0, b1; memcpy(&a0, &cj->ps0[i], 8); memcpy(&a1, &cj->ps1[i], 8); memcpy(&b0, &ca->ps0[i], 8); memcpy(&b1, &ca->ps1[i], 8); snprintf(why, sizeof why, "f%d jit=%016llX/%016llX ref=%016llX/%016llX", i, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)b0, (unsigned long long)b1); }
    if (!bad && cj->cr != ca->cr) { bad = true; snprintf(why, sizeof why, "cr jit=%08X ref=%08X", cj->cr, ca->cr); }
    if (!bad && cj->xer != ca->xer) { bad = true; snprintf(why, sizeof why, "xer jit=%08X ref=%08X", cj->xer, ca->xer); }
    if (!bad && cj->lr != ca->lr) { bad = true; snprintf(why, sizeof why, "lr jit=%08X ref=%08X", cj->lr, ca->lr); }
    if (!bad && cj->ctr != ca->ctr) { bad = true; snprintf(why, sizeof why, "ctr jit=%08X ref=%08X", cj->ctr, ca->ctr); }
    if (!bad && cj->pc != ca->pc) { bad = true; snprintf(why, sizeof why, "pc jit=%08X ref=%08X", cj->pc, ca->pc); }
    if (!bad && memcmp(busJ->ram, bA.ram, 0x400000)) { bad = true; snprintf(why, sizeof why, "ram differs"); }
    if (!bad && memcmp(busJ->lc, bA.lc, LC_SIZE)) { bad = true; snprintf(why, sizeof why, "lc differs"); }
    if (bad) {
      if (getenv("DBG")) for (int i = 0; i < 32; i++) printf("   r%d init=%08X jit=%08X ref=%08X\n", i, init.gpr[i], cj->gpr[i], ca->gpr[i]);
      fails++; printf("iter %d FAIL (%s) after %d instr, block:\n", it, why, cnt);
      for (int i = 0; i < K; i++) printf("  %08X %s\n", words[i], i < cnt ? "" : "(not run)");
    }
  }
  printf("%s: %d iterations, %d guest instructions checked, %d failures\n", fails ? "FAIL" : "PASS", iters, totalInstr, fails);
  return fails != 0;
}
