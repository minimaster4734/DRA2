#include "jit.h"
#include <cstddef>
#include <cstring>

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

namespace {
enum Cond { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13, AL = 14 };
enum Dp { AND = 0, EOR = 1, SUB = 2, RSB = 3, ADD = 4, ADC = 5, SBC = 6, TST = 8, TEQ = 9, CMP = 10, CMN = 11, ORR = 12, MOV = 13, BIC = 14, MVN = 15 };
enum Sh { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
enum { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC };

const u32 OFF_GPR = offsetof(CPU, gpr), OFF_PC = offsetof(CPU, pc), OFF_LR = offsetof(CPU, lr), OFF_CTR = offsetof(CPU, ctr),
          OFF_XER = offsetof(CPU, xer), OFF_CR = offsetof(CPU, cr), OFF_CNT = offsetof(CPU, jitCount), OFF_LIM = offsetof(CPU, jitLimit), OFF_FAST = offsetof(CPU, jitFast), OFF_PS0 = offsetof(CPU, ps0), OFF_PS1 = offsetof(CPU, ps1);

static inline u32 rotl32(u32 v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 mask32(int mb, int me) { u32 a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me); return mb <= me ? (a & b) : (a | b); }

struct Stub { size_t br; size_t cont; int kind; };   // slow-path memory access: kind = 0..2 load 8/16/32, 3..5 store 8/16/32

struct Asm {
  std::vector<u32>& c; const JitHelpers& hp; std::vector<Stub> stubs; std::vector<JitLink>* links = nullptr;
  Asm(std::vector<u32>& o, const JitHelpers& h) : c(o), hp(h) {}
  size_t here() const { return c.size(); }
  void e(u32 w) { c.push_back(w); }

  static bool enc(u32 v, u32& o) { for (int r = 0; r < 16; r++) { u32 x = rotl32(v, 2 * r); if (x < 256) { o = (r << 8) | x; return true; } } return false; }
  void dpRaw(int cond, int op, int s, int rn, int rd, u32 op2, int imm) { e((cond << 28) | (imm << 25) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | op2); }
  void dpReg(int op, int s, int rn, int rd, int rm, int sh = LSL, int amt = 0) { dpRaw(AL, op, s, rn, rd, rm | (sh << 5) | (amt << 7), 0); }
  void dpRegShReg(int op, int s, int rn, int rd, int rm, int sh, int rs) { dpRaw(AL, op, s, rn, rd, rm | (sh << 5) | 0x10 | (rs << 8), 0); }
  void movw(int rd, u32 v) { e(0xE3000000 | ((v >> 12) & 0xF) << 16 | rd << 12 | (v & 0xFFF)); }
  void movt(int rd, u32 v) { e(0xE3400000 | ((v >> 12) & 0xF) << 16 | rd << 12 | (v & 0xFFF)); }
  void movImm(int rd, u32 v, int cond = AL) {
    u32 x;
    if (enc(v, x)) dpRaw(cond, MOV, 0, 0, rd, x, 1);
    else if (enc(~v, x)) dpRaw(cond, MVN, 0, 0, rd, x, 1);
    else { movw(rd, v & 0xFFFF); if (v >> 16) movt(rd, v >> 16); }
  }
  // op rd, rn, #v (uses r12 when the immediate is not encodable). Handles ADD/SUB sign swap and AND/BIC inversion.
  void dpImm(int op, int s, int rn, int rd, u32 v) {
    u32 x;
    if (enc(v, x)) { dpRaw(AL, op, s, rn, rd, x, 1); return; }
    if (!s) {
      if (op == ADD && enc(-v, x)) { dpRaw(AL, SUB, 0, rn, rd, x, 1); return; }
      if (op == SUB && enc(-v, x)) { dpRaw(AL, ADD, 0, rn, rd, x, 1); return; }
      if (op == AND && enc(~v, x)) { dpRaw(AL, BIC, 0, rn, rd, x, 1); return; }
      if (op == BIC && enc(~v, x)) { dpRaw(AL, AND, 0, rn, rd, x, 1); return; }
    }
    movImm(R12, v); dpReg(op, s, rn, rd, R12);
  }
  void mov(int rd, int rm) { dpReg(MOV, 0, 0, rd, rm); }
  void ldrImm(int rt, int rn, u32 off) { e(0xE5900000 | rn << 16 | rt << 12 | off); }
  void strImm(int rt, int rn, u32 off) { e(0xE5800000 | rn << 16 | rt << 12 | off); }
  void ldrG(int rt, int n) { ldrImm(rt, R4, OFF_GPR + 4 * n); }
  void strG(int n, int rt) { strImm(rt, R4, OFF_GPR + 4 * n); }
  size_t branch(int cond) { e((cond << 28) | 0x0A000000); return here() - 1; }
  void patch(size_t br, size_t target) { c[br] = (c[br] & 0xFF000000) | (((int)target - (int)br - 2) & 0x00FFFFFF); }
  void branchTo(int cond, size_t target) { size_t b = branch(cond); patch(b, target); }
  void blxReg(int rm) { e(0xE12FFF30 | rm); }
  void call(u32 addr) { movImm(R12, addr); blxReg(R12); }
  void rev(int rd, int rm) { e(0xE6BF0F30 | rd << 12 | rm); }
  void rev16(int rd, int rm) { e(0xE6BF0FB0 | rd << 12 | rm); }
  void sxth(int rd, int rm) { e(0xE6BF0070 | rd << 12 | rm); }
  void sxtb(int rd, int rm) { e(0xE6AF0070 | rd << 12 | rm); }
  void ubfx(int rd, int rn, int lsb, int w) { e(0xE7E00050 | (w - 1) << 16 | rd << 12 | lsb << 7 | rn); }
  void clz(int rd, int rm) { e(0xE16F0F10 | rd << 12 | rm); }
  void mul(int rd, int rm, int rs) { e(0xE0000090 | rd << 16 | rs << 8 | rm); }
  void umull(int lo, int hi, int rm, int rs) { e(0xE0800090 | hi << 16 | lo << 12 | rs << 8 | rm); }
  void smull(int lo, int hi, int rm, int rs) { e(0xE0C00090 | hi << 16 | lo << 12 | rs << 8 | rm); }

  void prologue() { e(0xE92D5FF0); mov(R4, R0); mov(R5, R1); }
  void epilogue() { e(0xE8BD9FF0); }
  void addCount(int n) { ldrImm(R1, R4, OFF_CNT); dpImm(ADD, 0, R1, R1, n); strImm(R1, R4, OFF_CNT); }
  // constant-target exit: accumulate the retired count and, while under the budget, jump straight into the target block
  void exitConst(u32 target, int n) {
    addCount(n); ldrImm(R2, R4, OFF_LIM); dpReg(CMP, 1, R1, 0, R2);
    size_t bs = branch(HS); size_t lk = branch(AL);
    patch(bs, here()); patch(lk, here());
    movImm(R0, target); strImm(R0, R4, OFF_PC); epilogue();
    if (links) links->push_back({(u32)lk, target});
  }
  // indirect exit (blr/bctr...): next pc in r0; try the direct-mapped {pc, entry} table before returning to the dispatcher
  void exitInd(int n) {
    addCount(n); ldrImm(R2, R4, OFF_LIM); dpReg(CMP, 1, R1, 0, R2);
    size_t bs = branch(HS);
    ldrImm(R2, R4, OFF_FAST); ubfx(R3, R0, 2, 16); dpReg(ADD, 0, R2, R2, R3, LSL, 3);
    ldrImm(R3, R2, 0); dpReg(CMP, 1, R3, 0, R0); ldrImm(R12, R2, 4);
    size_t bn = branch(NE);
    e(0xE12FFF10 | R12);   // bx r12
    patch(bs, here()); patch(bn, here());
    strImm(R0, R4, OFF_PC); epilogue();
  }
  void exitReg(int n) { addCount(n); strImm(R0, R4, OFF_PC); epilogue(); }   // next pc already in r0

  // r3 = cr field value (4 bits) -> CR field f
  void insertCrf(int f, int rv) {
    int sh = 28 - 4 * f; ldrImm(R2, R4, OFF_CR);
    dpImm(BIC, 0, R2, R2, 0xFu << sh); dpReg(ORR, 0, R2, R2, rv, LSL, sh); strImm(R2, R4, OFF_CR);
  }
  // flags are set; build LT/GT/EQ nibble in r3 (signed or unsigned compare), add SO, insert into CR field f
  void crFromFlags(int f, bool sgn) {
    movImm(R3, 2);
    dpRaw(sgn ? LT : LO, MOV, 0, 0, R3, 8, 1); dpRaw(sgn ? GT : HI, MOV, 0, 0, R3, 4, 1);
    ldrImm(R2, R4, OFF_XER); dpImm(TST, 1, R2, 0, 0x80000000u);
    dpRaw(NE, ORR, 0, R3, R3, 1, 1);
    insertCrf(f, R3);
  }
  void cr0Of(int rv) { dpImm(CMP, 1, rv, 0, 0); if (rv != R0) {} crFromFlags(0, true); }

  void stubFor(size_t br, int kind) { stubs.push_back({br, 0, kind}); }
  void emitStubs() {
    for (auto& s : stubs) {
      patch(s.br, here());
      mov(R1, R0); mov(R0, R4);
      static const int ldk[3] = {0, 1, 2};
      u32 fn = s.kind == 0 ? hp.r8 : s.kind == 1 ? hp.r16 : s.kind == 2 ? hp.r32 : s.kind == 3 ? hp.w8 : s.kind == 4 ? hp.w16 : hp.w32;
      (void)ldk; call(fn);
      if (s.kind < 3) mov(R2, R0);
      branchTo(AL, s.cont);
    }
  }
  // ea in r0 -> checks RAM range; on miss branches to a stub. After this, r1 = offset into RAM.
  size_t rangeCheck() {
    dpImm(BIC, 0, R0, R1, 0xE0000000u); dpImm(CMP, 1, R1, 0, 0x1800000);
    dpRaw(LO, CMP, 1, R0, 0, 0x20E, 1);   // cmplo r0,#0xE0000000  (0xE ror 4)
    return branch(HS);
  }
};

struct Tr {
  Asm a; u32 pc0; const u32* w; int nw; int idx = 0;
  Tr(std::vector<u32>& o, const JitHelpers& h) : a(o, h) {}

#define OP_RD ((op >> 21) & 31)
#define OP_RS OP_RD
#define OP_RA ((op >> 16) & 31)
#define OP_RB ((op >> 11) & 31)
#define OP_RC (op & 1)
#define OP_SIMM ((s32)(s16)(op & 0xFFFF))
#define OP_UIMM (op & 0xFFFF)

  // effective address into r0 (and r6 for update forms)
  void eaD(u32 op, bool update) {
    int ra = OP_RA; s32 d = OP_SIMM;
    if (ra == 0) a.movImm(R0, d); else { a.ldrG(R0, ra); if (d) a.dpImm(ADD, 0, R0, R0, d); }
    if (update) a.mov(R6, R0);
  }
  void eaX(u32 op, bool update) {
    int ra = OP_RA, rb = OP_RB;
    if (ra == 0) a.ldrG(R0, rb); else { a.ldrG(R0, ra); a.ldrG(R1, rb); a.dpReg(ADD, 0, R0, R0, R1); }
    if (update) a.mov(R6, R0);
  }
  // loads: size 1/2/4, sign = lha. destination register rd.
  void load(int size, bool sign, int rd) { loadR2(size, sign); a.strG(rd, R2); }
  void loadR2(int size, bool sign) {   // ea in r0 -> value in r2
    size_t br = a.rangeCheck();
    if (size == 4) { a.e(0xE7900001 | R5 << 16 | R2 << 12); a.rev(R2, R2); }          // ldr r2,[r5,r1]
    else if (size == 2) { a.e(0xE1900000 | R5 << 16 | R2 << 12 | 0xB1); a.rev16(R2, R2); }  // ldrh r2,[r5,r1]
    else a.e(0xE7D00001 | R5 << 16 | R2 << 12);                                        // ldrb r2,[r5,r1]
    a.stubFor(br, size == 1 ? 0 : size == 2 ? 1 : 2); a.stubs.back().cont = a.here();
    if (sign) a.sxth(R2, R2);
  }
  void store(int size, int rs) { a.ldrG(R2, rs); storeR2(size); }
  void storeR2(int size) {   // ea in r0, value in r2
    size_t br = a.rangeCheck();
    if (size == 4) { a.rev(R3, R2); a.e(0xE7800001 | R5 << 16 | R3 << 12); }
    else if (size == 2) { a.rev16(R3, R2); a.e(0xE1800000 | R5 << 16 | R3 << 12 | 0xB1); }
    else a.e(0xE7C00001 | R5 << 16 | R2 << 12);
    a.stubFor(br, size == 1 ? 3 : size == 2 ? 4 : 5); a.stubs.back().cont = a.here();
  }


  // ---- floating-point loads/stores (integer-only conversions; rare cases fall back to the interpreter) ----
  void fpHelperCall(u32 op, u32 pc) { a.movImm(R1, op); a.movImm(R2, pc); a.mov(R0, R4); a.call(a.hp.exec); }
  void fpLoad(u32 op, u32 pc, bool dbl, bool update) {
    int fd = OP_RD; eaD(op, update);
    if (dbl) {
      a.mov(R7, R0); loadR2(4, false); a.mov(R8, R2); a.dpImm(ADD, 0, R7, R0, 4); loadR2(4, false);
      a.strImm(R2, R4, OFF_PS0 + 8 * fd); a.strImm(R8, R4, OFF_PS0 + 8 * fd + 4);
    } else {
      loadR2(4, false);
      a.ubfx(R3, R2, 23, 8); a.dpImm(SUB, 0, R3, R12, 1); a.dpImm(CMP, 1, R12, 0, 254);
      size_t brNormal = a.branch(LO);
      a.dpReg(MOV, 1, 0, R12, R2, LSL, 1);                      // movs r12, r2, lsl #1  (zero?)
      size_t brSlow = a.branch(NE);
      a.dpImm(AND, 0, R2, R3, 0x80000000u); a.movImm(R12, 0);   // +-0.0
      size_t brStore = a.branch(AL);
      a.patch(brNormal, a.here());
      a.dpImm(ADD, 0, R3, R3, 896); a.dpReg(MOV, 0, 0, R3, R3, LSL, 20);
      a.dpImm(AND, 0, R2, R12, 0x80000000u); a.dpReg(ORR, 0, R3, R3, R12);
      a.ubfx(R12, R2, 3, 20); a.dpReg(ORR, 0, R3, R3, R12);
      a.dpReg(MOV, 0, 0, R12, R2, LSL, 29);
      a.patch(brStore, a.here());
      a.strImm(R12, R4, OFF_PS0 + 8 * fd); a.strImm(R3, R4, OFF_PS0 + 8 * fd + 4);
      a.strImm(R12, R4, OFF_PS1 + 8 * fd); a.strImm(R3, R4, OFF_PS1 + 8 * fd + 4);
      size_t brEnd = a.branch(AL);
      a.patch(brSlow, a.here());
      fpHelperCall(op, pc);
      a.patch(brEnd, a.here());
    }
    if (update) a.strG(OP_RA, R6);
  }
  void fpStore(u32 op, u32 pc, bool dbl, bool update) {
    int fs = OP_RS; eaD(op, update);
    if (dbl) {
      a.mov(R7, R0); a.ldrImm(R2, R4, OFF_PS0 + 8 * fs + 4); storeR2(4);
      a.dpImm(ADD, 0, R7, R0, 4); a.ldrImm(R2, R4, OFF_PS0 + 8 * fs); storeR2(4);
    } else {
      a.ldrImm(R2, R4, OFF_PS0 + 8 * fs + 4); a.ldrImm(R3, R4, OFF_PS0 + 8 * fs);
      a.ubfx(R7, R2, 20, 11); a.dpImm(SUB, 0, R7, R8, 897); a.dpImm(CMP, 1, R8, 0, 253);
      size_t brNormal = a.branch(LO);
      a.dpReg(MOV, 1, 0, R8, R2, LSL, 1); a.dpReg(ORR, 1, R8, R8, R3);   // orrs r8, r8, r3  (zero?)
      size_t brSlow = a.branch(NE);
      a.dpImm(AND, 0, R2, R2, 0x80000000u);
      size_t brStore = a.branch(AL);
      a.patch(brNormal, a.here());
      a.dpImm(SUB, 0, R7, R7, 896); a.dpReg(MOV, 0, 0, R7, R7, LSL, 23);
      a.dpImm(AND, 0, R2, R8, 0x80000000u) /* sign -> r8 */; 
      // r8 = sign (computed from the original hi word in r2); r9 = mantissa pieces
      a.dpReg(ORR, 0, R7, R7, R8);
      a.ubfx(R8, R2, 0, 20); a.dpReg(ORR, 0, R7, R7, R8, LSL, 3);
      a.dpReg(ORR, 0, R7, R7, R3, LSR, 29);
      a.dpImm(BIC, 0, R3, R8, 0xE0000000u);
      a.dpImm(CMP, 1, R8, 0, 0x10000000);
      a.dpRaw(HI, MOV, 0, 0, R9, 1, 1); a.dpRaw(LO, MOV, 0, 0, R9, 0, 1); a.dpRaw(EQ, AND, 0, R7, R9, 1, 1);
      a.dpReg(ADD, 0, R7, R7, R9); a.mov(R2, R7);
      a.patch(brStore, a.here());
      storeR2(4);
      size_t brEnd = a.branch(AL);
      a.patch(brSlow, a.here());
      fpHelperCall(op, pc);
      a.patch(brEnd, a.here());
    }
    if (update) a.strG(OP_RA, R6);
  }

  // returns true if the instruction ended the block
  bool one(u32 op, u32 pc, int n /* instructions retired including this one */) {
    int opc = op >> 26;
    switch (opc) {
      case 14: { int ra = OP_RA; s32 d = OP_SIMM; if (!ra) a.movImm(R0, d); else { a.ldrG(R0, ra); a.dpImm(ADD, 0, R0, R0, d); } a.strG(OP_RD, R0); return false; }
      case 15: { int ra = OP_RA; u32 d = OP_UIMM << 16; if (!ra) a.movImm(R0, d); else { a.ldrG(R0, ra); a.dpImm(ADD, 0, R0, R0, d); } a.strG(OP_RD, R0); return false; }
      case 7: { a.ldrG(R0, OP_RA); a.movImm(R1, (u32)OP_SIMM); a.mul(R0, R0, R1); a.strG(OP_RD, R0); return false; }   // mulli
      case 24: case 25: case 26: case 27: {
        u32 imm = (opc & 1) ? OP_UIMM << 16 : OP_UIMM; if (opc == 24 && op == 0x60000000) return false;
        a.ldrG(R0, OP_RS); a.dpImm(opc < 26 ? ORR : EOR, 0, R0, R0, imm); a.strG(OP_RA, R0); return false; }
      case 28: case 29: { u32 imm = (opc & 1) ? OP_UIMM << 16 : OP_UIMM; a.ldrG(R0, OP_RS); a.dpImm(AND, 0, R0, R0, imm); a.strG(OP_RA, R0); a.cr0Of(R0); return false; }
      case 10: case 11: {
        a.ldrG(R0, OP_RA); u32 v = opc == 11 ? (u32)OP_SIMM : OP_UIMM; u32 x;
        if (Asm::enc(v, x)) a.dpRaw(AL, CMP, 1, R0, 0, x, 1); else { a.movImm(R1, v); a.dpReg(CMP, 1, R0, 0, R1); }
        a.crFromFlags((op >> 23) & 7, opc == 11); return false; }
      case 21: { // rlwinm
        u32 m = mask32((op >> 6) & 31, (op >> 1) & 31); int sh = (op >> 11) & 31;
        a.ldrG(R0, OP_RS); if (sh) a.dpReg(MOV, 0, 0, R0, R0, ROR, (32 - sh) & 31);
        if (m != 0xFFFFFFFFu) a.dpImm(AND, 0, R0, R0, m);
        a.strG(OP_RA, R0); if (OP_RC) a.cr0Of(R0); return false; }
      case 20: { // rlwimi
        u32 m = mask32((op >> 6) & 31, (op >> 1) & 31); int sh = (op >> 11) & 31;
        a.ldrG(R0, OP_RS); if (sh) a.dpReg(MOV, 0, 0, R0, R0, ROR, (32 - sh) & 31);
        a.dpImm(AND, 0, R0, R0, m); a.ldrG(R1, OP_RA); a.dpImm(BIC, 0, R1, R1, m); a.dpReg(ORR, 0, R0, R0, R1);
        a.strG(OP_RA, R0); if (OP_RC) a.cr0Of(R0); return false; }
      case 32: eaD(op, false); load(4, false, OP_RD); return false;
      case 33: eaD(op, true); load(4, false, OP_RD); a.strG(OP_RA, R6); return false;
      case 34: eaD(op, false); load(1, false, OP_RD); return false;
      case 35: eaD(op, true); load(1, false, OP_RD); a.strG(OP_RA, R6); return false;
      case 40: eaD(op, false); load(2, false, OP_RD); return false;
      case 41: eaD(op, true); load(2, false, OP_RD); a.strG(OP_RA, R6); return false;
      case 42: eaD(op, false); load(2, true, OP_RD); return false;
      case 43: eaD(op, true); load(2, true, OP_RD); a.strG(OP_RA, R6); return false;
      case 36: eaD(op, false); store(4, OP_RS); return false;
      case 37: eaD(op, true); store(4, OP_RS); a.strG(OP_RA, R6); return false;
      case 38: eaD(op, false); store(1, OP_RS); return false;
      case 39: eaD(op, true); store(1, OP_RS); a.strG(OP_RA, R6); return false;
      case 44: eaD(op, false); store(2, OP_RS); return false;
      case 45: eaD(op, true); store(2, OP_RS); a.strG(OP_RA, R6); return false;
      case 48: case 49: fpLoad(op, pc, false, opc == 49); return false;
      case 50: case 51: fpLoad(op, pc, true, opc == 51); return false;
      case 52: case 53: fpStore(op, pc, false, opc == 53); return false;
      case 54: case 55: fpStore(op, pc, true, opc == 55); return false;
      case 18: { // b / bl
        s32 li = (s32)(op & 0x03FFFFFC); if (li & 0x02000000) li |= 0xFC000000;
        u32 tgt = (op & 2) ? (u32)li : pc + li;
        if (op & 1) { a.movImm(R0, pc + 4); a.strImm(R0, R4, OFF_LR); }
        a.exitConst(tgt, n); return true; }
      case 16: { // bc
        int bo = (op >> 21) & 31, bi = (op >> 16) & 31; u32 tgt = (op & 2) ? (u32)(s32)(s16)(op & 0xFFFC) : pc + (u32)(s32)(s16)(op & 0xFFFC);
        if (op & 1) { a.movImm(R0, pc + 4); a.strImm(R0, R4, OFF_LR); }
        return cond(bo, bi, n, [&] { a.exitConst(tgt, n); });
      }
      case 19: {
        int xo = (op >> 1) & 0x3FF;
        if (xo == 16 || xo == 528) {
          int bo = (op >> 21) & 31, bi = (op >> 16) & 31; bool lk = op & 1;
          if (xo == 16) { a.ldrImm(R3, R4, OFF_LR); } else a.ldrImm(R3, R4, OFF_CTR);
          a.dpImm(BIC, 0, R3, R3, 3);                 // target in r3 (before LR update)
          if (lk) { a.movImm(R0, pc + 4); a.strImm(R0, R4, OFF_LR); }
          return cond(bo, bi, n, [&] { a.mov(R0, R3); a.exitInd(n); }, xo == 16);
        }
        if (xo == 150) return false;   // isync
        return helper(op, pc, n);
      }
      case 31: return op31(op, pc, n);
      default: return helper(op, pc, n);
    }
  }

  // emit branch condition test. onTaken emits the taken exit. Returns true if the block must end (unconditional).
  template <class F> bool cond(int bo, int bi, int n, F onTaken, bool ctrAllowed = true) {
    bool useCtr = !(bo & 4) && ctrAllowed, useCr = !(bo & 16);
    if (!useCtr && !useCr) { onTaken(); return true; }   // note: bcctr ignores the CTR bits (as the interpreter does)
    std::vector<size_t> fails;
    if (useCtr) {
      // r3 may hold the register target: use r1 for ctr
      a.ldrImm(R1, R4, OFF_CTR); a.dpImm(SUB, 0, R1, R1, 1); a.strImm(R1, R4, OFF_CTR);
      a.dpImm(CMP, 1, R1, 0, 0);
      fails.push_back(a.branch((bo & 2) ? NE : EQ));    // bo&2: branch if ctr == 0
    }
    if (useCr) {
      a.ldrImm(R1, R4, OFF_CR); a.dpImm(TST, 1, R1, 0, 1u << (31 - bi));
      fails.push_back(a.branch((bo & 8) ? EQ : NE));    // want bit set -> fail when clear (Z set)
    }
    onTaken();
    for (size_t f : fails) a.patch(f, a.here());
    return false;
  }

  bool helper(u32 op, u32 pc, int n) {
    a.movImm(R1, op); a.movImm(R2, pc); a.mov(R0, R4); a.call(a.hp.exec);
    int opc = op >> 26; int xo = (op >> 1) & 0x3FF;
    bool ends = opc == 17 || (opc == 19 && (xo == 50 || xo == 16 || xo == 528)) || (opc == 31 && (xo == 146 || xo == 467)) || opc == 0;
    if (opc == 31 && xo == 467) { int spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5); ends = spr == 22 || spr == 26 || spr == 27; }
    if (ends) { a.exitReg(n); return true; }
    return false;
  }

  bool op31(u32 op, u32 pc, int n) {
    int xo = (op >> 1) & 0x3FF; int rc = OP_RC; bool oe = (op >> 10) & 1;
    auto fin = [&](int rv) { a.strG(OP_RA, rv); if (rc) a.cr0Of(rv); };
    switch (xo) {
      case 0: case 32: { a.ldrG(R0, OP_RA); a.ldrG(R1, OP_RB); a.dpReg(CMP, 1, R0, 0, R1); a.crFromFlags((op >> 23) & 7, xo == 0); return false; }
      case 28: a.ldrG(R0, OP_RS); a.ldrG(R1, OP_RB); a.dpReg(AND, 0, R0, R0, R1); fin(R0); return false;
      case 60: a.ldrG(R0, OP_RS); a.ldrG(R1, OP_RB); a.dpReg(BIC, 0, R0, R0, R1); fin(R0); return false;
      case 316: a.ldrG(R0, OP_RS); a.ldrG(R1, OP_RB); a.dpReg(EOR, 0, R0, R0, R1); fin(R0); return false;
      case 444: a.ldrG(R0, OP_RS); a.ldrG(R1, OP_RB); a.dpReg(ORR, 0, R0, R0, R1); fin(R0); return false;
      case 124: a.ldrG(R0, OP_RS); a.ldrG(R1, OP_RB); a.dpReg(ORR, 0, R0, R0, R1); a.dpReg(MVN, 0, 0, R0, R0); fin(R0); return false;
      case 24: case 536: a.ldrG(R0, OP_RS); a.ldrG(R1, OP_RB); a.dpImm(AND, 0, R1, R1, 0x3F); a.dpRegShReg(MOV, 0, 0, R0, R0, xo == 24 ? LSL : LSR, R1); fin(R0); return false;
      case 26: a.ldrG(R0, OP_RS); a.clz(R0, R0); fin(R0); return false;
      case 824: {   // srawi
        int sh = OP_RB; a.ldrG(R0, OP_RS);
        if (sh == 0) { a.ldrImm(R3, R4, OFF_XER); a.dpImm(BIC, 0, R3, R3, 0x20000000u); a.strImm(R3, R4, OFF_XER); a.mov(R1, R0); fin(R1); return false; }
        a.dpReg(MOV, 0, 0, R1, R0, ASR, sh);
        a.dpImm(AND, 0, R0, R2, (1u << sh) - 1); a.dpImm(CMP, 1, R2, 0, 0); a.dpRaw(NE, MOV, 0, 0, R2, 1, 1);
        a.dpReg(AND, 0, R2, R2, R0, LSR, 31);
        a.ldrImm(R3, R4, OFF_XER); a.dpImm(BIC, 0, R3, R3, 0x20000000u); a.dpReg(ORR, 0, R3, R3, R2, LSL, 29); a.strImm(R3, R4, OFF_XER);
        fin(R1); return false; }
      case 922: a.ldrG(R0, OP_RS); a.sxth(R0, R0); fin(R0); return false;
      case 954: a.ldrG(R0, OP_RS); a.sxtb(R0, R0); fin(R0); return false;
      case 23: eaX(op, false); load(4, false, OP_RD); return false;
      case 55: eaX(op, true); load(4, false, OP_RD); a.strG(OP_RA, R6); return false;
      case 87: eaX(op, false); load(1, false, OP_RD); return false;
      case 119: eaX(op, true); load(1, false, OP_RD); a.strG(OP_RA, R6); return false;
      case 279: eaX(op, false); load(2, false, OP_RD); return false;
      case 311: eaX(op, true); load(2, false, OP_RD); a.strG(OP_RA, R6); return false;
      case 343: eaX(op, false); load(2, true, OP_RD); return false;
      case 375: eaX(op, true); load(2, true, OP_RD); a.strG(OP_RA, R6); return false;
      case 151: eaX(op, false); store(4, OP_RS); return false;
      case 183: eaX(op, true); store(4, OP_RS); a.strG(OP_RA, R6); return false;
      case 215: eaX(op, false); store(1, OP_RS); return false;
      case 247: eaX(op, true); store(1, OP_RS); a.strG(OP_RA, R6); return false;
      case 407: eaX(op, false); store(2, OP_RS); return false;
      case 439: eaX(op, true); store(2, OP_RS); a.strG(OP_RA, R6); return false;
      case 19: a.ldrImm(R0, R4, OFF_CR); a.strG(OP_RD, R0); return false;
      case 339: { int spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5);
        if (spr == 8 || spr == 9 || spr == 1) { a.ldrImm(R0, R4, spr == 8 ? OFF_LR : spr == 9 ? OFF_CTR : OFF_XER); a.strG(OP_RD, R0); return false; }
        return helper(op, pc, n); }
      case 467: { int spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5);
        if (spr == 8 || spr == 9 || spr == 1) { a.ldrG(R0, OP_RS); a.strImm(R0, R4, spr == 8 ? OFF_LR : spr == 9 ? OFF_CTR : OFF_XER); return false; }
        return helper(op, pc, n); }
      case 854: case 598: case 86: case 54: case 246: case 278: case 470: return false;   // sync/eieio/dcbf/dcbst/dcbtst/dcbt: no-ops here
      default: break;
    }
    if (!oe) {
      switch (xo & 0x1FF) {
        case 266: a.ldrG(R0, OP_RA); a.ldrG(R1, OP_RB); a.dpReg(ADD, 0, R0, R0, R1); fin2(op, R0); return false;
        case 40: a.ldrG(R0, OP_RB); a.ldrG(R1, OP_RA); a.dpReg(SUB, 0, R0, R0, R1); fin2(op, R0); return false;
        case 104: a.ldrG(R0, OP_RA); a.dpRaw(AL, RSB, 0, R0, R0, 0, 1); fin2(op, R0); return false;
        case 235: a.ldrG(R0, OP_RA); a.ldrG(R1, OP_RB); a.mul(R0, R0, R1); fin2(op, R0); return false;
        case 75: a.ldrG(R0, OP_RA); a.ldrG(R1, OP_RB); a.smull(R2, R0, R0, R1); fin2(op, R0); return false;
        case 11: a.ldrG(R0, OP_RA); a.ldrG(R1, OP_RB); a.umull(R2, R0, R0, R1); fin2(op, R0); return false;
        default: break;
      }
    }
    return helper(op, pc, n);
  }
  // arithmetic result lands in RD
  void fin2(u32 op, int rv) { a.strG(OP_RD, rv); if (OP_RC) a.cr0Of(rv); }
};
}  // namespace

int jitTranslate(std::vector<u32>& out, const JitHelpers& hp, u32 pc, const u32* words, int nwords, std::vector<JitLink>* links) {
  Tr t(out, hp); t.a.links = links; t.pc0 = pc; t.w = words; t.nw = nwords;
  t.a.prologue();
  int i = 0; bool ended = false;
  for (; i < nwords && !ended; i++) ended = t.one(words[i], pc + 4 * i, i + 1);
  if (!ended) t.a.exitConst(pc + 4 * i, i);
  t.a.emitStubs();
  return i;
}
