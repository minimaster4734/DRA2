// GX command-processor stream parser. Consumes bytes written to the write-gather pipe.
// Rendering hooks (GLES2) are attached later; this stage tracks state and raises PE events.
#include "gc.h"
#include "gl.h"
#include <cmath>

static inline u32 be32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }

static int fmtSize(int fmt) { static const int s[8] = {1, 1, 2, 2, 4, 4, 4, 4}; return s[fmt & 7]; }
static int colSize(int fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }

// Bytes per vertex given current VCD/VAT
static int vertexSize(Machine& m, int vat) {
  u32 lo = m.cpRegs[0x50], hi = m.cpRegs[0x60];
  u32 a = m.cpRegs[0x70 + vat], b = m.cpRegs[0x80 + vat], c = m.cpRegs[0x90 + vat];
  int sz = 0;
  if (lo & 1) sz += 1;
  for (int i = 0; i < 8; i++) if (lo & (2u << i)) sz += 1;
  auto idx = [&](int mode, int direct) { return mode == 0 ? 0 : mode == 1 ? direct : mode == 2 ? 1 : 2; };
  int pos = (lo >> 9) & 3, nrm = (lo >> 11) & 3, c0 = (lo >> 13) & 3, c1 = (lo >> 15) & 3;
  sz += idx(pos, ((a & 1) ? 3 : 2) * fmtSize((a >> 1) & 7));
  if (nrm) {
    int cnt = (a >> 9) & 1 ? 9 : 3;
    int direct = cnt * fmtSize((a >> 10) & 7);
    if (nrm >= 2) { int n = (cnt == 9 && ((a >> 31) & 1)) ? 3 : 1; sz += n * (nrm == 2 ? 1 : 2); } else sz += direct;
  }
  sz += idx(c0, colSize((a >> 14) & 7));
  sz += idx(c1, colSize((a >> 18) & 7));
  int tcnt[8], tfmt[8];
  tcnt[0] = (a >> 21) & 1; tfmt[0] = (a >> 22) & 7;
  tcnt[1] = b & 1; tfmt[1] = (b >> 1) & 7;
  tcnt[2] = (b >> 9) & 1; tfmt[2] = (b >> 10) & 7;
  tcnt[3] = (b >> 18) & 1; tfmt[3] = (b >> 19) & 7;
  tcnt[4] = (b >> 27) & 1; tfmt[4] = (b >> 28) & 7;
  tcnt[5] = (c >> 5) & 1; tfmt[5] = (c >> 6) & 7;
  tcnt[6] = (c >> 14) & 1; tfmt[6] = (c >> 15) & 7;
  tcnt[7] = (c >> 23) & 1; tfmt[7] = (c >> 24) & 7;
  for (int i = 0; i < 8; i++) {
    int mode = (hi >> (i * 2)) & 3;
    sz += idx(mode, (tcnt[i] + 1) * fmtSize(tfmt[i]));
  }
  return sz;
}


// ---- vertex attribute decoding ----
static const u8* arrayPtr(Machine& m, int attr, u32 idx) {
  u32 base = m.cpRegs[0xA0 + attr] & 0x1FFFFFFF, stride = m.cpRegs[0xB0 + attr] & 0xFF;
  u64 a = (u64)base + (u64)idx * stride; return (a < 0xFFFFFFFFull && inRamPhys((u32)a, 16)) ? m.bus.ram + a : nullptr;
}
static float rdComp(const u8*& p, int fmt, int shift) {
  float v;
  switch (fmt) {
    case 0: v = p[0]; p += 1; break; case 1: v = (s8)p[0]; p += 1; break;
    case 2: v = (u16)(p[0] << 8 | p[1]); p += 2; break; case 3: v = (s16)(p[0] << 8 | p[1]); p += 2; break;
    default: { u32 u = be32(p); memcpy(&v, &u, 4); p += 4; return v; }
  }
  return shift ? v / (float)(1 << shift) : v;
}
static void rdColor(const u8*& p, int fmt, float* o) {
  o[3] = 1.f;
  switch (fmt) {
    case 0: { u16 v = be16(p); p += 2; o[0] = ((v >> 11) & 31) / 31.f; o[1] = ((v >> 5) & 63) / 63.f; o[2] = (v & 31) / 31.f; break; }
    case 1: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; p += 3; break;
    case 2: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; p += 4; break;
    case 3: { u16 v = be16(p); p += 2; o[0] = ((v >> 12) & 15) / 15.f; o[1] = ((v >> 8) & 15) / 15.f; o[2] = ((v >> 4) & 15) / 15.f; o[3] = (v & 15) / 15.f; break; }
    case 4: { u32 v = p[0] << 16 | p[1] << 8 | p[2]; p += 3; o[0] = ((v >> 18) & 63) / 63.f; o[1] = ((v >> 12) & 63) / 63.f; o[2] = ((v >> 6) & 63) / 63.f; o[3] = (v & 63) / 63.f; break; }
    default: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; o[3] = p[3] / 255.f; p += 4; break;
  }
}

static void decodeVertices(Machine& m, int vat, const u8* d, int cnt, std::vector<GXVertex>& out) {
  u32 lo = m.cpRegs[0x50], hi = m.cpRegs[0x60];
  u32 a = m.cpRegs[0x70 + vat], b = m.cpRegs[0x80 + vat], c = m.cpRegs[0x90 + vat];
  int posMode = (lo >> 9) & 3, nrmMode = (lo >> 11) & 3, colMode[2] = {(int)((lo >> 13) & 3), (int)((lo >> 15) & 3)};
  int posCnt = (a & 1) ? 3 : 2, posFmt = (a >> 1) & 7, posShift = (a >> 4) & 31;
  int nrmCnt = (a >> 9) & 1 ? 9 : 3, nrmFmt = (a >> 10) & 7, colFmt[2] = {(int)((a >> 14) & 7), (int)((a >> 18) & 7)};
  bool nrmIdx3 = (a >> 31) & 1;
  int tCnt[8], tFmt[8], tSh[8];
  tCnt[0] = (a >> 21) & 1; tFmt[0] = (a >> 22) & 7; tSh[0] = (a >> 25) & 31;
  tCnt[1] = b & 1; tFmt[1] = (b >> 1) & 7; tSh[1] = (b >> 4) & 31;
  tCnt[2] = (b >> 9) & 1; tFmt[2] = (b >> 10) & 7; tSh[2] = (b >> 13) & 31;
  tCnt[3] = (b >> 18) & 1; tFmt[3] = (b >> 19) & 7; tSh[3] = (b >> 22) & 31;
  tCnt[4] = (b >> 27) & 1; tFmt[4] = (b >> 28) & 7; tSh[4] = c & 31;
  tCnt[5] = (c >> 5) & 1; tFmt[5] = (c >> 6) & 7; tSh[5] = (c >> 9) & 31;
  tCnt[6] = (c >> 14) & 1; tFmt[6] = (c >> 15) & 7; tSh[6] = (c >> 18) & 31;
  tCnt[7] = (c >> 23) & 1; tFmt[7] = (c >> 24) & 7; tSh[7] = (c >> 27) & 31;
  int nrmShift = nrmFmt == 1 ? 6 : (nrmFmt == 3 ? 14 : 0);
  int vs = vertexSize(m, vat);
  out.resize(cnt);
  for (int i = 0; i < cnt; i++) {
    GXVertex& v = out[i]; memset(&v, 0, sizeof v);
    const u8* p = d + (size_t)i * vs;
    if (lo & 1) { v.pmi = *p++; v.hasPmi = true; }
    for (int k = 0; k < 8; k++) if (lo & (2u << k)) { v.tmi[k] = *p++; v.hasTmi[k] = true; }
    auto fetchIdx = [&](int mode) -> u32 { u32 id; if (mode == 2) { id = *p++; } else { id = be16(p); p += 2; } return id; };
    // position
    { const u8* q = p; if (posMode >= 2) { u32 id = fetchIdx(posMode); q = arrayPtr(m, 0, id); }
      if (q) { v.pos[0] = rdComp(q, posFmt, posShift); v.pos[1] = rdComp(q, posFmt, posShift); if (posCnt == 3) v.pos[2] = rdComp(q, posFmt, posShift); }
      if (posMode == 1) p = q ? q : p; }
    // normal
    if (nrmMode) {
      const u8* q = p; bool direct = nrmMode == 1;
      if (!direct) { u32 id = fetchIdx(nrmMode); q = arrayPtr(m, 1, id); if (nrmCnt == 9 && nrmIdx3) { fetchIdx(nrmMode); fetchIdx(nrmMode); } }
      if (q) { const u8* r = q; for (int k = 0; k < 3; k++) v.nrm[k] = rdComp(r, nrmFmt, nrmShift); if (direct) { for (int k = 3; k < nrmCnt; k++) rdComp(r, nrmFmt, nrmShift); p = r; } }
      v.hasNrm = true;
    }
    for (int cch = 0; cch < 2; cch++) if (colMode[cch]) {
      const u8* q = p; if (colMode[cch] >= 2) { u32 id = fetchIdx(colMode[cch]); q = arrayPtr(m, 2 + cch, id); }
      if (q) { const u8* r = q; rdColor(r, colFmt[cch], v.col[cch]); if (colMode[cch] == 1) p = r; }
      v.hasCol[cch] = true;
    }
    for (int k = 0; k < 8; k++) { int mode = (hi >> (k * 2)) & 3; if (!mode) continue;
      const u8* q = p; if (mode >= 2) { u32 id = fetchIdx(mode); q = arrayPtr(m, 4 + k, id); }
      if (q) { const u8* r = q; v.tex[k][0] = rdComp(r, tFmt[k], tSh[k]); if (tCnt[k]) v.tex[k][1] = rdComp(r, tFmt[k], tSh[k]); if (mode == 1) p = r; }
      v.hasTex[k] = true; }
  }
}

void Machine::gxBP(u32 reg, u32 val) {
  { static int k=0; if (GENV("BPLOG") && cpu.icount>2615000000ull && k++<400) log("BP %02X <- %06X", reg, val); }
  if (reg == 0xFE) { bpMask = val & 0xFFFFFF; return; }          // BP mask: applies to the next write only
  if (bpMask != 0xFFFFFF) { val = (bpRegs[reg] & ~bpMask) | (val & bpMask); bpMask = 0xFFFFFF; }
  bpRegs[reg] = val;
  switch (reg) {
    case 0x45:   // PE_DONE (draw done / finish)
      gxStats.finish++; if (GENV("FINLOG") && cpu.icount > 2590000000ull && cpu.icount < 2700000000ull) log("FINISH cmd val=%06X pe0A=%04X", val, peReg[0x0A>>1]); finishDue = cpu.icount + (GENV("FDELAY") ? atoi(GENV("FDELAY")) : 40000); break;   // GPU latency: interrupt arrives later
    case 0x47:   // PE_TOKEN
      gxStats.token = val & 0xFFFF; peReg[0x0E >> 1] = (u16)val; break;
    case 0x48:   // PE_TOKEN_INT
      gxStats.token = val & 0xFFFF; peReg[0x0E >> 1] = (u16)val; peReg[0x0A >> 1] |= 4; updateInts(); break;
    case 0x52: gxStats.efbCopies++; if (rd) rd->efbCopy(val); break;   // EFB copy (XFB or texture)
    case 0x64: tlutRamAddr = (val & 0x1FFFFF) << 5; break;
    case 0x65: if (rd) rd->tlutLoad(tlutRamAddr, val & 0x3FF, (val >> 10) & 0x3FF); break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: {
      int r = (reg - 0xE0) >> 1; bool hi = reg & 1, konst = (val >> 23) & 1;
      auto sx = [&](u32 v) { int s = (int)(v & 0x7FF); if (s & 0x400) s -= 0x800; return s; };
      float a = konst ? (float)(val & 0xFF) / 255.f : sx(val) / 255.f, b = konst ? (float)((val >> 12) & 0xFF) / 255.f : sx(val >> 12) / 255.f;
      float (*dst)[4] = konst ? tevKonst : tevCol;
      if (!hi) { dst[r][0] = a; dst[r][3] = b; } else { dst[r][2] = a; dst[r][1] = b; }
      break; }
    default: break;
  }
}

size_t Machine::gxParse(const u8* d, size_t n, size_t p, int depth) {
  while (p < n) {
    u8 cmd = d[p];
    if (GENV("GXTRACE") && cpu.icount > 14184700000ull && cpu.icount < 14184784000ull && cmd != 0) log("GXT depth=%d cmd=%02X b1=%02X", depth, cmd, p+1<n ? d[p+1] : 0);
    size_t need = 1, len = 0;
    switch (cmd) {
      case 0x00: case 0x44: case 0x48: p++; continue;   // NOP, INVL_VC
      case 0x08: need = 6; if (p + need > n) goto done; if (GENV("CPLOG") && ((d[p+1] & 0xF) < 8 && d[p+1] >= 0x70 && d[p+1] < 0xA0)) log("CP %02X <- %08X", d[p+1], be32(d+p+2)); cpRegs[d[p + 1]] = be32(d + p + 2); if (d[p + 1] == 0x30) xf[0x1018] = cpRegs[0x30]; else if (d[p + 1] == 0x40) xf[0x1019] = cpRegs[0x40]; gxStats.cpLoads++; p += need; continue;
      case 0x10: { need = 5; if (p + need > n) goto done; u32 h = be32(d + p + 1); u32 cnt = (h >> 16) + 1; need = 5 + cnt * 4; if (p + need > n) goto done;
        u32 addr = h & 0xFFFF; for (u32 i = 0; i < cnt; i++) if (addr + i < 0x1100) xf[addr + i] = be32(d + p + 5 + i * 4); gxStats.xfLoads++; p += need; continue; }
      case 0x20: case 0x28: case 0x30: case 0x38: { need = 5; if (p + need > n) goto done; gxStats.indexedXf++;
        u32 v = be32(d + p + 1); int arr = 12 + ((cmd >> 3) & 3); u32 idx = v >> 16, cnt = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
        const u8* q = arrayPtr(*this, arr, idx);
        if (q) for (u32 i = 0; i < cnt; i++) if (addr + i < 0x1100) xf[addr + i] = be32(q + i * 4);
        p += need; continue; }
      case 0x40: { need = 9; if (p + need > n) goto done; gxStats.callDL++;
        u32 addr = be32(d + p + 1) & 0x1FFFFFFF, size = be32(d + p + 5);
        if (GENV("DLLOG") && size > 40 && inRamPhys(addr, size) && (bus.ram[addr]==0x99 || bus.ram[addr+1]==0x99)) { static int ln=0; if (ln++ < 40) log("CALLDL addr=%08X size=%X first=%02X %02X %02X %02X %02X %02X %02X %02X", addr, size, bus.ram[addr],bus.ram[addr+1],bus.ram[addr+2],bus.ram[addr+3],bus.ram[addr+4],bus.ram[addr+5],bus.ram[addr+6],bus.ram[addr+7]); }
        if (depth < 4 && inRamPhys(addr, size)) gxParse(bus.ram + addr, size, 0, depth + 1);
        p += need; continue; }
      case 0x61: need = 5; if (p + need > n) goto done; { u32 v = be32(d + p + 1); gxBP(v >> 24, v & 0xFFFFFF); } p += need; continue;
      default:
        if (cmd >= 0x80) {
          need = 3; if (p + need > n) goto done;
          int vat = cmd & 7, cnt = be16(d + p + 1), vs = vertexSize(*this, vat);
          need = 3 + (size_t)cnt * vs; if (p + need > n) goto done;
          gxStats.draws++; gxStats.verts += cnt;
          if (rd && cnt && !rd->skipNow) { static std::vector<GXVertex> vtmp; decodeVertices(*this, vat, d + p + 3, cnt, vtmp); if (GENV("VATLOG") && cpRegs[0xA0]==(u32)strtoul(GENV("VATLOG"),0,16) && 1) { static int kk=0; if (kk++<2) { static bool dumped=false; if (!dumped && GENV("DUMPARR")) { dumped=true; FILE* f=fopen(GENV("DUMPARR"),"wb"); u32 rg[16*2]; for (int r=0;r<16;r++){rg[r*2]=cpRegs[0xA0+r]; rg[r*2+1]=cpRegs[0xB0+r];} fwrite(rg,4,32,f); u32 cc=cnt; fwrite(&cc,4,1,f); fwrite(d+p+3,1,cnt*8,f); for (int r=0;r<16;r++){ u32 b=cpRegs[0xA0+r]&0x1FFFFFFF; u32 L=0x10000; if (b+L>RAM_SIZE) L=0; fwrite(&b,4,1,f); fwrite(&L,4,1,f); if (L) fwrite(bus.ram+b,1,L,f);} fclose(f);} } { for (int r = 0; r < 16; r++) log("ARRREG %X base=%08X stride=%X", r, cpRegs[0xA0+r], cpRegs[0xB0+r]); } { for (int e = (int)be16(d+p+3) - 1; e < (int)be16(d+p+3) + 5; e++) { const u8* q = arrayPtr(*this, 0, e); u32 u[3]; for (int k = 0; k < 3; k++) u[k] = be32(q + 4*k); float f[3]; memcpy(f, u, 12); log("ENT %X f=(%g %g %g) s16=(%d %d %d %d %d %d)", e, f[0], f[1], f[2], (s16)be16(q), (s16)be16(q+2), (s16)be16(q+4), (s16)be16(q+6), (s16)be16(q+8), (s16)be16(q+10)); } } { const u8* q0 = arrayPtr(*this, 0, 0x1A2); const u8* q1 = arrayPtr(*this, 0, 0x1A1); log("ARRDATA %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X | %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X", q0[0],q0[1],q0[2],q0[3],q0[4],q0[5],q0[6],q0[7],q0[8],q0[9],q0[10],q0[11], q1[0],q1[1],q1[2],q1[3],q1[4],q1[5],q1[6],q1[7],q1[8],q1[9],q1[10],q1[11]); } log("CMD=%02X ARR0 base=%08X stride=%X idx=%04X %04X %04X %04X %04X %04X", (unsigned)cmd, cpRegs[0xA0], cpRegs[0xB0], be16(d+p+3), be16(d+p+3+8), be16(d+p+3+16), be16(d+p+3+24), be16(d+p+3+32), be16(d+p+3+40)); if (GENV("VATLOG") && cnt==atoi(GENV("VATLOG")) && 1) log("VAT vat=%d vcd=%08X/%08X a=%08X b=%08X c=%08X vs=%d raw=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", vat, cpRegs[0x50], cpRegs[0x60], cpRegs[0x70+vat], cpRegs[0x80+vat], cpRegs[0x90+vat], vertexSize(*this,vat), d[p+3],d[p+4],d[p+5],d[p+6],d[p+7],d[p+8],d[p+9],d[p+10],d[p+11],d[p+12],d[p+13],d[p+14],d[p+15],d[p+16],d[p+17],d[p+18],d[p+19],d[p+20],d[p+21],d[p+22],d[p+23],d[p+24],d[p+25],d[p+26],d[p+27],d[p+28],d[p+29],d[p+30]); } if (GENV("GXFMT")) log("GXFMT cnt=%d cmd=%02X vat=%d a=%08X posbase=%08X stride=%X vcd=%08X/%08X", (int)cnt, (unsigned)cmd, vat, cpRegs[0x70+vat], cpRegs[0xA0], cpRegs[0xB0], cpRegs[0x50], cpRegs[0x60]); rd->draw((cmd >> 3) & 7, vtmp.data(), cnt); }
          p += need; continue;
        }
        { char b[80]; snprintf(b, sizeof b, "GX: bad command 0x%02X at stream pos %zu", cmd, p); cpu.halt(b); return n; }
    }
  }
done:
  return p;
}

void Machine::gxProcess() {
  gxPos = gxParse(gxfifo.data(), gxfifo.size(), gxPos, 0);
  if (gxPos > (1u << 22) && gxPos == gxfifo.size()) { gxfifo.clear(); gxPos = 0; }   // keep the buffer small
}
