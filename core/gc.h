// Clean-room GameCube runtime core (no Nintendo or game code included).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef uint8_t u8;  typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

static inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
static inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
static inline u64 bswap64(u64 v) { return __builtin_bswap64(v); }

enum : u32 {
  RAM_SIZE = 0x01800000,   // 24 MB main memory
  ARAM_SIZE = 0x01000000,  // 16 MB aux memory
  LC_BASE = 0xE0000000,    // locked cache
  LC_SIZE = 0x4000,
};

// ---------------- Memory / bus -----------------
#define GENV(name) ([]() -> const char* { static const char* v = getenv(name); return v; }())
extern bool g_watchOn;
struct Bus {
  u8* ram; u8* aram; u8* lc;
  Bus();
  ~Bus();
  // translated accessors (big endian semantics)
  u8  r8(u32 ea);  u16 r16(u32 ea);  u32 r32(u32 ea);  u64 r64(u32 ea);
  void w8(u32 ea, u8 v); void w16(u32 ea, u16 v); void w32(u32 ea, u32 v); void w64(u32 ea, u64 v);
  // inline fast paths (cached RAM / uncached mirror); everything else falls back to the full accessors
  static constexpr u32 FAST_LIM = 0xE0000000u;
  inline u8  fr8(u32 ea)  { u32 p = ea & 0x1FFFFFFFu; if (__builtin_expect(ea < FAST_LIM && p < RAM_SIZE, 1)) return ram[p]; return r8(ea); }
  inline u16 fr16(u32 ea) { u32 p = ea & 0x1FFFFFFFu; if (__builtin_expect(ea < FAST_LIM && p < RAM_SIZE, 1)) { u16 v; memcpy(&v, ram + p, 2); return __builtin_bswap16(v); } return r16(ea); }
  inline u32 fr32(u32 ea) { u32 p = ea & 0x1FFFFFFFu; if (__builtin_expect(ea < FAST_LIM && p < RAM_SIZE, 1)) { u32 v; memcpy(&v, ram + p, 4); return __builtin_bswap32(v); } return r32(ea); }
  inline void fw8(u32 ea, u8 v)   { u32 p = ea & 0x1FFFFFFFu; if (__builtin_expect(ea < FAST_LIM && p < RAM_SIZE, 1)) { ram[p] = v; return; } w8(ea, v); }
  inline void fw16(u32 ea, u16 v) { u32 p = ea & 0x1FFFFFFFu; if (__builtin_expect(ea < FAST_LIM && p < RAM_SIZE, 1)) { v = __builtin_bswap16(v); memcpy(ram + p, &v, 2); return; } w16(ea, v); }
  inline void fw32(u32 ea, u32 v) { u32 p = ea & 0x1FFFFFFFu; if (__builtin_expect(ea < FAST_LIM && p < RAM_SIZE && !g_watchOn, 1)) { v = __builtin_bswap32(v); memcpy(ram + p, &v, 4); return; } w32(ea, v); }
  u8* ptr(u32 ea);  // direct RAM pointer or nullptr
  // hardware register space (phys 0x0C000000..0x0C00FFFF)
  u32 hwRead(u32 off, int size);
  void hwWrite(u32 off, u32 val, int size);
  // wiring
  struct Machine* m = nullptr;
};

// ---------------- CPU -----------------
struct CPU {
  u32 gpr[32] = {};
  double ps0[32] = {}, ps1[32] = {};
  u32 pc = 0, lr = 0, ctr = 0, xer = 0, cr = 0, msr = 0, srr0 = 0, srr1 = 0, fpscr = 0;
  u32 jitFast = 0;    // address of the {pc, entry} table used by indirect exits
  u32 jitLimit = 0;   // chained blocks keep running while jitCount < jitLimit
  u32 jitCount = 0;   // guest instructions retired by the last JIT block (kept within the first 4 KB: used as an A32 immediate offset)
  u32 sprs[1024] = {};
  bool decPending = false;
  u32 reserve = 0; bool reserveValid = false;
  u32 sr[16] = {};
  u32 dec = 0xFFFFFFFF;
  struct Spin { u32 pc; s32 r13off; bool whileZero; }; Spin spins[4] = {}; int nspins = 0;
  bool spinHit();
  u32 idlePc = 0;   // per-game: head of the OS idle loop; the CPU fast-forwards to the next hardware event there
  u64 tb = 0;
  u64 icount = 0;
  bool halted = false;
  std::string haltReason;
  u32 pcHist[64]; int pcHistPos = 0;
  Bus* bus = nullptr;
  struct Machine* m = nullptr;

  void step(int n);          // run up to n instructions
  void stepJit(int n);
  void exception(u32 vec);
  void checkInterrupts();
  void halt(const char* why) { halted = true; haltReason = why; }
  // implemented in cpu.cpp
  void exec(u32 op, u32& npc);
  void execPS(u32 op);
  void execFP(u32 op);
};

// ---------------- Machine -----------------
struct DiscFile { std::string path; u32 off, size; };

struct Machine {
  Bus bus;
  CPU cpu;
  FILE* iso = nullptr;
  std::vector<u8> fst;
  std::vector<DiscFile> files;
  // PI / interrupt state
  u32 piIntCause = 0, piIntMask = 0;
  // generic shadow regs for unimplemented hardware
  u32 shadow[0x4000 / 4 * 4] = {};
  bool trace = false;
  bool loadIso(const char* path);
  bool boot();
  void log(const char* fmt, ...);
  void updateInts();
  u32 exiRead(u32 off);
  void exiWrite(u32 off, u32 val);
  void tick();                      // called between CPU slices
  u64 nextVi = 6000000;
  u16 viReg[0x80] = {};             // 16-bit VI registers
  u32 viDi[4] = {};
  u16 dspReg[0x80] = {};            // DSP interface registers
  u32 dspMailToCpu = 0, dspMailToDsp = 0; bool dspBooted = false;
  std::vector<u8> gxfifo;           // bytes written to the write-gather pipe
  u16 cpReg[0x80] = {}, peReg[0x80] = {}, miReg[0x80] = {}, aiReg[0x20] = {};
  u32 exiReg[3][5] = {}, siReg[0x40] = {};
  u32 aiCountBase = 0; u64 aiStart = 0;
  // GX command processor state
  size_t gxPos = 0; u32 bpMask = 0xFFFFFF;
  struct Renderer* rd = nullptr;
  float tevCol[4][4] = {}, tevKonst[4][4] = {};
  u32 tlutRamAddr = 0;
  u16 padBtn = 0; u8 padStick[6] = {128, 128, 128, 128, 0, 0};   // buttons: A1 B2 X4 Y8 Start10 L100 R200 D400 U800 Z1000(hw order, see siPollResp)
  u64 viCount = 0;
  void siPoll();
  void siTransfer();
  void siUpdate();
  u32 siRead(u32 off);
  void siWrite(u32 off, u32 val);
  u32 cpRegs[256] = {}, bpRegs[256] = {}, xf[0x1100] = {};
  struct { u64 finish = 0, token = 0, efbCopies = 0, cpLoads = 0, xfLoads = 0, indexedXf = 0, callDL = 0, draws = 0, verts = 0; } gxStats;
  void gxProcess();
  size_t gxParse(const u8* d, size_t n, size_t p, int depth);
  void dspMailIn(u32 mail);
  void axRunList(u32 addr, u32 words);
  void dspPushMail(u32 mail, bool irq = false);
  u64 dspRearm = 0;
  u64 cardIntDue = 0;   // memory card completion interrupt (EXI0 EXIINT)
  u64 nextEvent() const { u64 e = nextVi; for (u64 v : {finishDue, axYieldDue, aiDmaDue, dspRearm, cardIntDue}) if (v && v < e) e = v; return e; }
  u64 finishDue = 0;
  u64 axYieldDue = 0; u64 aiDmaDue = 0; u64 aiDmaCount = 0;
  std::vector<s16> audioOut;   // interleaved stereo samples produced by AI DMA (consumed by the platform layer)
  void gxBP(u32 reg, u32 val);
  // DVD
  u32 diReg[0x40 / 4] = {};
  void diExec(u32 cmdBit);
  bool readDisc(u64 off, void* dst, u32 len);
};

enum : u32 {  // PI interrupt cause bits
  INT_ERROR = 1 << 0, INT_RSW = 1 << 1, INT_DI = 1 << 2, INT_SI = 1 << 3, INT_EXI = 1 << 4,
  INT_AI = 1 << 5, INT_DSP = 1 << 6, INT_MEM = 1 << 7, INT_VI = 1 << 8, INT_PE_TOKEN = 1 << 9,
  INT_PE_FINISH = 1 << 10, INT_CP = 1 << 11, INT_DEBUG = 1 << 12, INT_HSP = 1 << 13,
};
