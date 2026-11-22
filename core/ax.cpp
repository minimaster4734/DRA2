// High-level emulation of the GameCube "AX" DSP microcode (command-list mixer).
// Written from the documented behaviour of the protocol; no Nintendo code is included.
// Voices read their sample data from ARAM, the 5 ms mix is written back to main RAM and
// then picked up by the game through the AI DMA.
#include "gc.h"
#include <algorithm>

namespace {

constexpr int FRAME = 160;   // 5 ms @ 32 kHz

struct AXState {
  int buf[9][FRAME];         // mainL mainR mainS auxAL auxAR auxAS auxBL auxBR auxBS
  u32 compressorPos = 0;
} g;

enum { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S };

// ---- big-endian helpers on main RAM ----
inline u8* ramp(Machine& m, u32 a, u32 len) { a &= 0x1FFFFFFF; return (u64)a + len <= RAM_SIZE ? m.bus.ram + a : nullptr; }
inline u16 rd16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
inline void wr16(u8* p, u16 v) { p[0] = v >> 8; p[1] = (u8)v; }
inline s32 rd32s(const u8* p) { return (s32)((u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); }
inline void wr32s(u8* p, s32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
inline s16 clamp16(s64 v) { return (s16)std::max<s64>(-0x8000, std::min<s64>(0x7FFF, v)); }

void upload(Machine& m, u32 addr, int* b, int n = FRAME) { u8* p = ramp(m, addr, n * 4); if (!p) return; for (int i = 0; i < n; i++) wr32s(p + i * 4, b[i]); }
void downloadAdd(Machine& m, u32 addr, int* b) { u8* p = ramp(m, addr, FRAME * 4); if (!p) return; for (int i = 0; i < FRAME; i++) b[i] += rd32s(p + i * 4); }

// ---- parameter block word offsets (16-bit words) ----
enum {
  PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SEL = 5, PB_MIXCTRL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8,
  PB_MIX = 9,        // 9 x {volume, delta}: mainL mainR auxAL auxAR auxBL auxBR auxBS mainS auxAS
  PB_UPD_NUM = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
  PB_DPOP = 41,      // mainL auxAL auxBL mainR auxAR auxBR mainS auxAS auxBS
  PB_ENV_VOL = 50, PB_ENV_DELTA = 51,
  PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60, PB_CUR_HI = 61, PB_CUR_LO = 62,
  PB_COEFS = 63, PB_GAIN = 79, PB_PRED = 80, PB_YN1 = 81, PB_YN2 = 82,
  PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
  PB_LP_PRED = 90, PB_LP_YN1 = 91, PB_LP_YN2 = 92,
  PB_LPF_ON = 93, PB_LPF_YN1 = 94, PB_LPF_A0 = 95, PB_LPF_B0 = 96, PB_LOOPCNT = 97,
  PB_WORDS = 122,
};

struct Voice {
  u16 w[PB_WORDS];
  u32 start, end, cur; u16 fmt; s16 yn1, yn2; u16 pred; s16 gain;
  bool stopped;
  u32 u32of(int hi) const { return ((u32)w[hi] << 16) | w[hi + 1]; }
};

inline u8 aramByte(Machine& m, u32 a) { return m.bus.aram[a & (ARAM_SIZE - 1)]; }

// Fetch one decoded sample from the voice's sample stream (ARAM).
s16 nextSample(Machine& m, Voice& v) {
  if (v.stopped) return 0;
  int size = v.fmt & 3, decode = (v.fmt >> 2) & 3, gscale = (v.fmt >> 4) & 3;
  s32 raw = 0;
  if (size == 0) { u8 b = aramByte(m, v.cur >> 1); raw = (v.cur & 1) ? (b & 0xF) : (b >> 4); }
  else if (size == 1) raw = (s8)aramByte(m, v.cur);
  else if (size == 2) raw = (s16)(aramByte(m, v.cur * 2) << 8 | aramByte(m, v.cur * 2 + 1));
  int ci = (v.pred >> 4) & 7;
  s32 c1 = (s16)v.w[PB_COEFS + ci * 2], c2 = (s16)v.w[PB_COEFS + ci * 2 + 1];
  s16 out = 0; u32 step = 2;
  if (decode == 0) {   // ADPCM
    raw &= 0xF; if (raw >= 8) raw -= 16;
    s32 val = ((1 << (v.pred & 0xF)) * raw) + ((0x400 + c1 * v.yn1 + c2 * v.yn2) >> 11);
    out = clamp16(val); v.yn2 = v.yn1; v.yn1 = out; v.cur += 1;
    if ((v.end & 0xF) == 0 && v.cur == v.end) v.cur = v.start + 1;
    else if ((v.end & 0xF) == 1 && v.cur == v.end - 1) v.cur = v.start;
    else if ((v.cur & 15) == 0) { v.pred = aramByte(m, (v.cur & ~15u) >> 1) & 0x7F; v.cur += 2; step += 2; }
  } else {
    int sh = gscale == 0 ? 11 : gscale == 1 ? 0 : 16;
    s32 val = (((s32)v.gain * raw) >> sh) + (((c1 * v.yn1) >> sh) + ((c2 * v.yn2) >> sh));
    out = (s16)val; v.yn2 = v.yn1; v.yn1 = out; v.cur += 1;
  }
  if (v.cur == v.end + step - 1) {   // reached the end of the sample data
    v.cur = v.start;
    if (v.w[PB_LOOPING]) {
      v.pred = v.w[PB_LP_PRED] & 0x7F;
      if (v.w[PB_IS_STREAM] != 1) { v.yn1 = (s16)v.w[PB_LP_YN1]; v.yn2 = (s16)v.w[PB_LP_YN2]; } else v.w[PB_LOOPCNT]++;
    } else { v.w[PB_RUNNING] = 0; v.stopped = true; }
  }
  return out;
}

void mixAdd(int* out, const s16* in, int n, u16* vol, u16 delta, bool ramp, s16* dpop) {
  for (int i = 0; i < n; i++) {
    s16 s = clamp16(((s64)in[i] * (s16)vol[0]) >> 15);
    // volume registers are unsigned 16-bit fractions (0x8000 == 1.0)
    s = clamp16(((s64)in[i] * vol[0]) >> 15);
    out[i] += s; if (ramp) vol[0] += delta; *dpop = s;
  }
}

void processVoiceList(Machine& m, u32 pbAddr) {
  while (pbAddr) {
    u8* p = ramp(m, pbAddr, PB_WORDS * 2); if (!p) break;
    Voice v; for (int i = 0; i < PB_WORDS; i++) v.w[i] = rd16(p + i * 2);
    // per-ms parameter updates
    u16 updates[64] = {}; u32 upAddr = v.u32of(PB_UPD_HI);
    if (upAddr) { u8* q = ramp(m, upAddr, 128); if (q) for (int i = 0; i < 64; i++) updates[i] = rd16(q + i * 2); }
    u32 mc = v.w[PB_MIXCTRL];
    for (int ms = 0; ms < 5; ms++) {
      u32 startIdx = 0; for (int i = 0; i < ms; i++) startIdx += v.w[PB_UPD_NUM + i];
      u32 cnt = v.w[PB_UPD_NUM + ms];
      if (startIdx < 32 && cnt <= 32 - startIdx)
        for (u32 i = startIdx; i < startIdx + cnt; i++) { u16 off = updates[i * 2], val = updates[i * 2 + 1]; if (off < PB_WORDS) v.w[off] = val; }
      if (v.w[PB_RUNNING] != 1) continue;
      mc = v.w[PB_MIXCTRL];
      v.start = v.u32of(PB_LOOP_HI) & 0x3FFFFFFF; v.end = v.u32of(PB_END_HI) & 0x3FFFFFFF; v.cur = v.u32of(PB_CUR_HI) & 0xBFFFFFFF;
      v.fmt = v.w[PB_FORMAT]; v.yn1 = (s16)v.w[PB_YN1]; v.yn2 = (s16)v.w[PB_YN2]; v.pred = v.w[PB_PRED] & 0x7F; v.gain = (s16)v.w[PB_GAIN]; v.stopped = false;
      // sample-rate conversion
      s16 smp[32]; s16* last = (s16*)&v.w[PB_LAST]; u32 pos = v.w[PB_FRAC], ratio = v.u32of(PB_RATIO_HI);
      s16 hist[2] = {(s16)v.w[PB_LAST], (s16)v.w[PB_LAST + 1]};
      int type = v.w[PB_SRC_TYPE];
      for (int i = 0; i < 32; i++) {
        if (type == 2) { smp[i] = nextSample(m, v); continue; }
        pos += ratio;
        while (pos >= 0x10000) { hist[0] = hist[1]; hist[1] = nextSample(m, v); pos -= 0x10000; }
        u32 fr = pos & 0xFFFF;
        smp[i] = fr ? (s16)(((s32)hist[0] * (s32)(0x10000 - fr) + (s32)hist[1] * (s32)fr) >> 16) : hist[1];
      }
      (void)last; v.w[PB_LAST] = (u16)hist[0]; v.w[PB_LAST + 1] = (u16)hist[1];
      v.w[PB_FRAC] = (u16)pos;
      v.w[PB_CUR_HI] = (u16)(v.cur >> 16); v.w[PB_CUR_LO] = (u16)v.cur;
      v.w[PB_YN1] = (u16)v.yn1; v.w[PB_YN2] = (u16)v.yn2; v.w[PB_PRED] = v.pred;
      // volume envelope
      for (int i = 0; i < 32; i++) {
        s32 vol = (s16)v.w[PB_ENV_VOL];
        smp[i] = clamp16(((s32)smp[i] * vol) >> 15);
        v.w[PB_ENV_VOL] = (u16)(v.w[PB_ENV_VOL] + v.w[PB_ENV_DELTA]);
      }
      if (v.w[PB_LPF_ON]) for (int i = 0; i < 32; i++) { s16 y = clamp16(((s32)(u16)v.w[PB_LPF_A0] * smp[i] + (s32)(s16)v.w[PB_LPF_B0] * (s16)v.w[PB_LPF_YN1]) >> 15); v.w[PB_LPF_YN1] = (u16)y; smp[i] = y; }
      struct Ch { int bufIdx, mixOff, mcBit, rampBit, dpopOff; };
      static const Ch chans[9] = {
        {MAIN_L, 0, 0x0001, 0x0008, 0}, {MAIN_R, 2, 0x0002, 0x0008, 3}, {MAIN_S, 14, 0x0004, 0x0008, 6},
        {AUXA_L, 4, 0x0010, 0x0040, 1}, {AUXA_R, 6, 0x0020, 0x0040, 4}, {AUXA_S, 16, 0x0080, 0x0100, 7},
        {AUXB_L, 8, 0x0200, 0x0800, 2}, {AUXB_R, 10, 0x0400, 0x0800, 5}, {AUXB_S, 12, 0x1000, 0x2000, 8},
      };
      for (auto& c : chans) {
        if (!(mc & c.mcBit)) continue;
        u16* vol = &v.w[PB_MIX + c.mixOff];
        s16 dp = 0;
        mixAdd(g.buf[c.bufIdx] + ms * 32, smp, 32, vol, vol[1], (mc & c.rampBit) != 0, &dp);
        v.w[PB_DPOP + c.dpopOff] = (u16)dp;
      }
    }
    for (int i = 0; i < PB_WORDS; i++) wr16(p + i * 2, v.w[i]);
    pbAddr = ((u32)v.w[PB_NEXT_HI] << 16) | v.w[PB_NEXT_LO];
  }
}

void setup(Machine& m, u32 addr) {
  u8* p = ramp(m, addr, 9 * 6); if (!p) { memset(g.buf, 0, sizeof g.buf); return; }
  for (int i = 0; i < 9; i++) {
    s32 value = (s32)((u32)rd16(p + i * 6) << 16 | rd16(p + i * 6 + 2)); s16 delta = (s16)rd16(p + i * 6 + 4);
    if (!value) memset(g.buf[i], 0, sizeof g.buf[i]); else for (int j = 0; j < FRAME; j++) g.buf[i][j] = value + j * delta;
  }
}

void downloadAndVolMix(Machine& m, u32 addr, u16 vm, u16 va, u16 vb) {
  u8* p = ramp(m, addr, 3 * 3 * FRAME * 4); if (!p) return;
  u16 vols[3] = {vm, va, vb};
  for (int s = 0; s < 3; s++) for (int c = 0; c < 3; c++) {
    int* b = g.buf[s * 3 + c];
    for (int i = 0; i < FRAME; i++) b[i] += (s32)(((s64)rd32s(p) * vols[s]) >> 15), p += 4;
  }
}

void outputSamples(Machine& m, u32 lrAddr, u32 surAddr) {
  upload(m, surAddr, g.buf[MAIN_S]);
  u8* p = ramp(m, lrAddr, FRAME * 4); if (!p) return;
  for (int i = 0; i < FRAME; i++) { wr16(p + i * 4, (u16)clamp16(g.buf[MAIN_R][i])); wr16(p + i * 4 + 2, (u16)clamp16(g.buf[MAIN_L][i])); }
}

void compressor(Machine& m, u16 threshold, u16 release, u32 table) {
  bool trig = false;
  for (int i = 0; i < FRAME; i++) if (abs(g.buf[MAIN_L][i]) > (int)threshold || abs(g.buf[MAIN_R][i]) > (int)threshold) { trig = true; break; }
  u32 frameBytes = FRAME * 2, off;
  if (trig) { off = g.compressorPos * frameBytes; g.compressorPos = release; }
  else if (g.compressorPos) { g.compressorPos--; off = (11 + g.compressorPos) * frameBytes; }
  else return;
  u8* p = ramp(m, table + off, FRAME * 2); if (!p) return;
  for (int i = 0; i < FRAME; i++) { u16 c = rd16(p + i * 2); g.buf[MAIN_L][i] = (int)(((s64)g.buf[MAIN_L][i] * c) >> 15); g.buf[MAIN_R][i] = (int)(((s64)g.buf[MAIN_R][i] * c) >> 15); }
}

}  // namespace

void Machine::axRunList(u32 addr, u32 words) {
  std::vector<u16> cl; auto load = [&](u32 a, u32 n) { cl.assign(n, 0); u8* p = ramp(*this, a, n * 2); if (p) for (u32 i = 0; i < n; i++) cl[i] = rd16(p + i * 2); };
  if (words > 4096) return;
  load(addr, words);
  u32 pb = 0; size_t i = 0; int guard = 0;
  auto A = [&](size_t& k) { u32 v = ((u32)cl[k] << 16) | cl[k + 1]; k += 2; return v; };
  while (i < cl.size() && guard++ < 4096) {
    u16 cmd = cl[i++];
    switch (cmd) {
      case 0x00: { u32 a = A(i); setup(*this, a); break; }
      case 0x01: { u32 a = A(i); u16 vm = cl[i], va = cl[i + 1], vb = cl[i + 2]; i += 3; downloadAndVolMix(*this, a, vm, va, vb); break; }
      case 0x02: pb = A(i); break;
      case 0x03: processVoiceList(*this, pb); break;
      case 0x04: case 0x05: { u32 w = A(i), r = A(i); int* b = g.buf[cmd == 4 ? AUXA_L : AUXB_L];
        if (w) { upload(*this, w, b); upload(*this, w + 640, b + FRAME); upload(*this, w + 1280, b + 2 * FRAME); }   // note: buffers L,R,S are consecutive
        downloadAdd(*this, r, g.buf[MAIN_L]); downloadAdd(*this, r + 640, g.buf[MAIN_R]); downloadAdd(*this, r + 1280, g.buf[MAIN_S]); break; }
      case 0x06: { u32 a = A(i); upload(*this, a, g.buf[MAIN_L]); upload(*this, a + 640, g.buf[MAIN_R]); upload(*this, a + 1280, g.buf[MAIN_S]); break; }
      case 0x07: { u32 a = A(i); u8* p = ramp(*this, a, FRAME * 4); if (p) for (int k = 0; k < FRAME; k++) { s32 s = rd32s(p + k * 4); g.buf[MAIN_L][k] = s; g.buf[MAIN_R][k] = s; g.buf[MAIN_S][k] = 0; } break; }
      case 0x08: i += 10; break;
      case 0x09: { u32 r = A(i); downloadAdd(*this, r, g.buf[MAIN_L]); downloadAdd(*this, r + 640, g.buf[MAIN_R]); downloadAdd(*this, r + 1280, g.buf[MAIN_S]); break; }
      case 0x0A: case 0x0B: case 0x0C: break;
      case 0x0D: { u32 a = A(i); u16 n = cl[i++]; if (n > 4096) return; load(a, n); i = 0; break; }
      case 0x0E: { u32 sur = A(i), lr = A(i); outputSamples(*this, lr, sur); break; }
      case 0x0F: i = cl.size(); break;
      case 0x10: { u32 ul = A(i), dl = A(i); upload(*this, ul, g.buf[AUXB_L]); upload(*this, ul + 640, g.buf[AUXB_R]);
        u8* p = ramp(*this, dl, 2 * FRAME * 4); if (p) { for (int k = 0; k < FRAME; k++) { s32 s = rd32s(p + k * 4); g.buf[AUXB_L][k] = s; g.buf[MAIN_L][k] += s; }
          for (int k = 0; k < FRAME; k++) { s32 s = rd32s(p + (FRAME + k) * 4); g.buf[AUXB_R][k] = s; g.buf[MAIN_R][k] += s; } } break; }
      case 0x11: { u32 a = A(i); u8* p = ramp(*this, a, FRAME * 4); if (p) for (int k = 0; k < FRAME; k++) { s32 s = rd32s(p + k * 4); g.buf[MAIN_L][k] = -s; g.buf[MAIN_R][k] = s; g.buf[MAIN_S][k] = 0; } break; }
      case 0x12: { u16 th = cl[i], rel = cl[i + 1]; i += 2; u32 t = A(i); compressor(*this, th, rel, t); break; }
      case 0x13: { u32 auxaUp = A(i), auxbSUp = A(i), mlDl = A(i), mrDl = A(i), blDl = A(i), brDl = A(i);
        upload(*this, auxaUp, g.buf[AUXA_L]); upload(*this, auxaUp + 640, g.buf[AUXA_R]); upload(*this, auxaUp + 1280, g.buf[AUXA_S]);
        upload(*this, auxbSUp, g.buf[AUXB_S]);
        downloadAdd(*this, mlDl, g.buf[MAIN_L]); downloadAdd(*this, mrDl, g.buf[MAIN_R]); downloadAdd(*this, blDl, g.buf[AUXB_L]); downloadAdd(*this, brDl, g.buf[AUXB_R]); break; }
      default: if (GENV("AXLOG")) log("AX: unknown command %04X", cmd); return;
    }
  }
}
