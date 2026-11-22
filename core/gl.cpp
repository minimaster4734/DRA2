#include <chrono>
// GX -> OpenGL ES 2.0 renderer (clean-room). Vertex transform/lighting/texgen on the CPU,
// TEV compiled to GLSL ES 1.00 fragment shaders, GameCube texture decoding, EFB as an FBO.
#include "gl.h"
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <vector>
static bool g_touchUi = false;
static void drawTouchOverlay(int w, int h);

enum { EFB_W = 640, EFB_H = 528 };

static inline float xff(Machine& m, u32 i) { u32 v = m.xf[i]; float f; memcpy(&f, &v, 4); return f; }

struct Prog { GLuint p = 0; GLint uR = -1, uK = -1, uAref = -1, uS[8]; };
struct TexEntry { GLuint tex = 0; u32 hash = 0; u64 hashFrame = ~0ull; int w = 0, h = 0, mips = 0; bool efb = false; };

struct Renderer::Impl {
  SDL_Window* win = nullptr; SDL_GLContext ctx = nullptr;
  GLuint fbo = 0, fboTex = 0, fboDepth = 0, vbo = 0;
  GLuint presentProg = 0, copyProg = 0, copyFbo = 0;
  GLuint epxProg = 0, epxTex = 0, epxFbo = 0; bool highp = false; int fboW = EFB_W, fboH = EFB_H; float sc = 1.f;
  std::unordered_map<std::string, Prog> progs;
  std::unordered_map<u64, TexEntry> texCache;
  std::unordered_map<u32, TexEntry> efbTex;     // by destination address
  u16 tlut[0x10000] = {};
  u32 tlutAddr = 0;
  std::vector<float> vb;
  int winW = 640, winH = 480;
  bool shown = true;
};

// ---------------------------------------------------------------- GL helpers
static GLuint compile(GLenum type, const std::string& src) {
  GLuint s = glCreateShader(type); const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); fprintf(stderr, "shader error: %s\n--- source ---\n%s\n", log, src.c_str()); }
  return s;
}
static GLuint link(const std::string& vs, const std::string& fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, vs)); glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
  glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_c0"); glBindAttribLocation(p, 2, "a_c1");
  for (int i = 0; i < 5; i++) glBindAttribLocation(p, 3 + i, ("a_t" + std::to_string(i)).c_str());
  glLinkProgram(p);
  GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); fprintf(stderr, "link error: %s\n", log); }
  return p;
}

bool Renderer::init(Machine* mach, bool showWindow) {
#ifdef __ANDROID__
  g_touchUi = true;
#endif
  if (getenv("TOUCH")) g_touchUi = true;
  m = mach; d = new Impl(); d->shown = showWindow;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return false; }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  d->win = SDL_CreateWindow("DRA2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, d->winW, d->winH,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (showWindow ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN));
  if (!d->win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return false; }
  d->ctx = SDL_GL_CreateContext(d->win);
  if (!d->ctx) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); return false; }
  SDL_GL_SetSwapInterval(0);
  fprintf(stderr, "GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

  glGenTextures(1, &d->fboTex); glBindTexture(GL_TEXTURE_2D, d->fboTex);
  { float s = std::min(1.0f, std::max(0.25f, renderScale)); d->sc = s; d->fboW = std::max(64, (int)(EFB_W * s + 0.5f)); d->fboH = std::max(64, (int)(EFB_H * s + 0.5f)); if (s != 1.0f) fprintf(stderr, "internal resolution %dx%d (%.0f%%)\n", d->fboW, d->fboH, s * 100); }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d->fboW, d->fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &d->fboDepth); glBindRenderbuffer(GL_RENDERBUFFER, d->fboDepth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, d->fboW, d->fboH);
  glGenFramebuffers(1, &d->fbo); glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->fboTex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d->fboDepth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "EFB framebuffer incomplete\n"); return false; }
  glGenBuffers(1, &d->vbo);
  glGenFramebuffers(1, &d->copyFbo);

  const char* pvs = "attribute vec2 a_pos; varying vec2 v_uv; void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }";
  { GLint rg[2] = {0, 0}, pr = 0; glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, rg, &pr); d->highp = pr != 0; }
  const char* hdr = "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";
  // present pass: bilinear, optional sharp-bilinear (crisp texels, smooth edges) and optional scanlines on the source line grid
  std::string pfs = std::string(hdr) +
    "varying vec2 v_uv; uniform sampler2D s0; uniform vec2 u_size; uniform float u_lines, u_scale, u_sharp, u_scan;\n"
    "void main(){ vec2 uv = v_uv;\n"
    " if (u_sharp > 0.5) { vec2 pix = uv * u_size; vec2 fl = floor(pix); vec2 f = pix - fl; vec2 r = clamp((f - 0.5) * u_scale + 0.5, 0.0, 1.0); uv = (fl + r) / u_size; }\n"
    " vec3 c = texture2D(s0, uv).rgb;\n"
    " if (u_scan > 0.5) { float fy = fract(v_uv.y * u_lines); c *= (0.62 + 0.38 * sin(3.14159265 * fy)) * 1.12; }\n"
    " gl_FragColor = vec4(c, 1.0); }";
  d->presentProg = link(pvs, pfs.c_str());
  // Scale2x / EPX into a texture of twice the EFB size (A = up, B = right, C = left, D = down)
  std::string efs = std::string(hdr) +
    "varying vec2 v_uv; uniform sampler2D s0; uniform vec2 u_src;\n"
    "bool eq(vec3 a, vec3 b){ vec3 d = abs(a - b); return max(d.r, max(d.g, d.b)) < 0.06; }\n"
    "void main(){ vec2 p = floor(v_uv * u_src * 2.0); vec2 t = floor(p * 0.5); vec2 q = p - t * 2.0; vec2 px = 1.0 / u_src; vec2 c = (t + 0.5) * px;\n"
    " vec3 P = texture2D(s0, c).rgb, A = texture2D(s0, c + vec2(0.0, px.y)).rgb, D = texture2D(s0, c - vec2(0.0, px.y)).rgb,\n"
    "      C = texture2D(s0, c - vec2(px.x, 0.0)).rgb, B = texture2D(s0, c + vec2(px.x, 0.0)).rgb; vec3 r = P;\n"
    " if (q.y > 0.5) { if (q.x < 0.5) { if (eq(C, A) && !eq(C, D) && !eq(A, B)) r = A; } else { if (eq(A, B) && !eq(A, C) && !eq(B, D)) r = B; } }\n"
    " else { if (q.x < 0.5) { if (eq(D, C) && !eq(D, B) && !eq(C, A)) r = C; } else { if (eq(B, D) && !eq(B, A) && !eq(D, C)) r = D; } }\n"
    " gl_FragColor = vec4(r, 1.0); }";
  d->epxProg = link(pvs, efs.c_str());
  glGenTextures(1, &d->epxTex); glBindTexture(GL_TEXTURE_2D, d->epxTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d->fboW * 2, d->fboH * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &d->epxFbo); glBindFramebuffer(GL_FRAMEBUFFER, d->epxFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->epxTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "Scale2x framebuffer incomplete; filter disabled\n"); d->highp = false; }
  glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
  if (!d->highp) fprintf(stderr, "note: no highp fragment precision, output filters disabled\n");
  // EFB -> texture copy conversion: u_mode 0 = RGBA, 1 = intensity (I4/I8), 2 = IA, 3 = alpha only, 4 = RGB (alpha forced 1)
  const char* cfs = "precision mediump float; varying vec2 v_uv; uniform sampler2D s0; uniform int u_mode;"
    "void main(){ vec4 c = texture2D(s0, v_uv); float y = dot(c.rgb, vec3(0.299,0.587,0.114));"
    " if (u_mode==1) gl_FragColor = vec4(y,y,y,y); else if (u_mode==2) gl_FragColor = vec4(y,y,y,c.a);"
    " else if (u_mode==3) gl_FragColor = vec4(c.a); else if (u_mode==4) gl_FragColor = vec4(c.rgb,1.0); else gl_FragColor = c; }";
  d->copyProg = link(pvs, cfs);
  return true;
}

void Renderer::shutdown() { if (d) { SDL_GL_DeleteContext(d->ctx); SDL_DestroyWindow(d->win); SDL_Quit(); } }

// ---------------------------------------------------------------- texture decoding
static inline u8 e3(u32 v) { return (v << 5) | (v << 2) | (v >> 1); }
static inline u8 e4(u32 v) { return v * 17; }
static inline u8 e5(u32 v) { return (v << 3) | (v >> 2); }
static inline u8 e6(u32 v) { return (v << 2) | (v >> 4); }
static inline u16 rd16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static inline void putpx(u8* o, u8 r, u8 g, u8 b, u8 a) { o[0] = r; o[1] = g; o[2] = b; o[3] = a; }
static void rgb565(u16 v, u8* o) { putpx(o, e5(v >> 11), e6((v >> 5) & 63), e5(v & 31), 255); }
static void rgb5a3(u16 v, u8* o) {
  if (v & 0x8000) putpx(o, e5((v >> 10) & 31), e5((v >> 5) & 31), e5(v & 31), 255);
  else putpx(o, e4((v >> 8) & 15), e4((v >> 4) & 15), e4(v & 15), e3((v >> 12) & 7));
}
static void palEntry(u16 v, int tf, u8* o) {
  if (tf == 0) putpx(o, v & 255, v & 255, v & 255, v >> 8);         // IA8: alpha high byte, intensity low byte
  else if (tf == 1) rgb565(v, o); else rgb5a3(v, o);
}

static int blockW(int f) { switch (f) { case 0: case 8: case 14: return 8; case 1: case 2: case 9: return 8; default: return 4; } }
static int blockH(int f) { switch (f) { case 0: case 8: case 14: return 8; case 1: case 2: case 9: return 4; default: return 4; } }
static int blockBytes(int f) { return f == 6 ? 64 : 32; }

static size_t levelBytes(int fmt, int w, int h) {
  int bw = blockW(fmt), bh = blockH(fmt);
  return (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * blockBytes(fmt);
}

static void decodeLevel(const u8* src, size_t avail, int fmt, int w, int h, const u16* tlut, int tf, u8* out) {
  int bw = blockW(fmt), bh = blockH(fmt), bb = blockBytes(fmt);
  int nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
  for (int by = 0; by < nby; by++) for (int bx = 0; bx < nbx; bx++) {
    size_t off = ((size_t)by * nbx + bx) * bb; if (off + bb > avail) return;
    const u8* p = src + off;
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
      int px = bx * bw + x, py = by * bh + y; if (px >= w || py >= h) continue;
      u8* o = out + ((size_t)py * w + px) * 4;
      switch (fmt) {
        case 0: { u8 b = p[y * 4 + x / 2]; u8 i = e4((x & 1) ? (b & 15) : (b >> 4)); putpx(o, i, i, i, i); break; }
        case 1: { u8 i = p[y * 8 + x]; putpx(o, i, i, i, i); break; }
        case 2: { u8 b = p[y * 8 + x]; u8 i = e4(b & 15), a = e4(b >> 4); putpx(o, i, i, i, a); break; }
        case 3: { const u8* q = p + (y * 4 + x) * 2; putpx(o, q[1], q[1], q[1], q[0]); break; }
        case 4: rgb565(rd16(p + (y * 4 + x) * 2), o); break;
        case 5: rgb5a3(rd16(p + (y * 4 + x) * 2), o); break;
        case 6: { int i = y * 4 + x; putpx(o, p[i * 2 + 1], p[32 + i * 2], p[32 + i * 2 + 1], p[i * 2]); break; }
        case 8: { u8 b = p[y * 4 + x / 2]; int idx = (x & 1) ? (b & 15) : (b >> 4); palEntry(tlut[idx], tf, o); break; }
        case 9: palEntry(tlut[p[y * 8 + x]], tf, o); break;
        case 10: palEntry(tlut[rd16(p + (y * 4 + x) * 2) & 0x3FFF], tf, o); break;
        case 14: {
          const u8* blk = p + ((y >> 2) * 2 + (x >> 2)) * 8; int sx = x & 3, sy = y & 3;
          u16 c0 = rd16(blk), c1 = rd16(blk + 2); u8 col[4][4]; rgb565(c0, col[0]); rgb565(c1, col[1]);
          if (c0 > c1) { for (int k = 0; k < 3; k++) { col[2][k] = (2 * col[0][k] + col[1][k]) / 3; col[3][k] = (col[0][k] + 2 * col[1][k]) / 3; } col[2][3] = col[3][3] = 255; }
          else { for (int k = 0; k < 3; k++) { col[2][k] = (col[0][k] + col[1][k]) / 2; col[3][k] = 0; } col[2][3] = 255; col[3][3] = 0; }
          int idx = (blk[4 + sy] >> (6 - 2 * sx)) & 3; memcpy(o, col[idx], 4); break; }
        default: putpx(o, 255, 0, 255, 255);
      }
    }
  }
}

void Renderer::tlutLoad(u32 ramAddr, u32 tmemOff, u32 count) {
  u32 n = count * 16; u32 base = tmemOff * 16;
  for (u32 i = 0; i < n && base + i < 0x10000; i++) {
    u32 a = ramAddr + i * 2; if (a + 2 > RAM_SIZE) break;
    d->tlut[base + i] = (u16)(m->bus.ram[a] << 8 | m->bus.ram[a + 1]);
  }
}

static u32 quickHash(const u8* p, size_t n) {
  u32 h = 2166136261u;
  size_t step = n > 4096 ? (n / 1024) & ~3u : 4; if (!step) step = 4;
  for (size_t i = 0; i + 4 <= n; i += step) { u32 v; memcpy(&v, p + i, 4); h = (h ^ v) * 16777619u; }
  return h ^ (u32)n;
}

static GLenum wrapMode(int w) { return w == 1 ? GL_REPEAT : (w == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE); }

// bind GameCube texture map `i` to texture unit `i`
static void bindMap(Machine& m, Renderer::Impl* d, u64 frame, int i) {
  int sreg = i < 4 ? 0x80 + i : 0xA0 + (i - 4);
  u32 mode0 = m.bpRegs[sreg], mode1 = m.bpRegs[sreg + 4], img0 = m.bpRegs[sreg + 8], img3 = m.bpRegs[sreg + 0x14];
  int w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
  u32 addr = (img3 & 0xFFFFFF) << 5;
  int minf = (mode0 >> 5) & 7, magf = (mode0 >> 4) & 1;
  bool wantMips = (minf & 3) != 0 && minf != 4 && w > 1 | h > 1 ? (minf == 1 || minf == 2 || minf == 5 || minf == 6) : false;
  int tlutReg = i < 4 ? 0x98 + i : 0xB8 + (i - 4);
  u32 tl = m.bpRegs[tlutReg]; u32 tlOff = tl & 0x3FF; int tf = (tl >> 10) & 3;
  glActiveTexture(GL_TEXTURE0 + i);
  TexEntry* e = nullptr;
  auto ef = d->efbTex.find(addr);
  if (ef != d->efbTex.end() && ef->second.w == w && ef->second.h == h) e = &ef->second;
  else {
    u64 key = ((u64)addr << 28) ^ ((u64)fmt << 24) ^ ((u64)w << 12) ^ h ^ ((u64)tlOff << 44) ^ ((u64)tf << 56) ^ (wantMips ? 1ull << 58 : 0);
    TexEntry& t = d->texCache[key]; e = &t;
    const u8* src = addr < RAM_SIZE ? m.bus.ram + addr : nullptr;
    if (!src) { if (!t.tex) { glGenTextures(1, &t.tex); } }
    else {
      size_t avail = RAM_SIZE - addr; size_t span = std::min(avail, levelBytes(fmt, w, h) * (wantMips ? 2 : 1));
      u32 hh = quickHash(src, span) ^ (fmt >= 8 && fmt <= 10 ? quickHash((const u8*)&d->tlut[tlOff * 16], 512) : 0);
      if (!t.tex || (t.hashFrame != frame && t.hash != hh)) {
        if (!t.tex) glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        std::vector<u8> px; int lw = w, lh = h, lvl = 0; const u8* p = src; size_t left = avail;
        int maxLod = wantMips ? 11 : 1;
        while (lvl < maxLod) {
          px.assign((size_t)lw * lh * 4, 0);
          decodeLevel(p, left, fmt, lw, lh, &d->tlut[tlOff * 16], tf, px.data());
          glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
          size_t lb = levelBytes(fmt, lw, lh); if (lb > left) break; p += lb; left -= lb; lvl++;
          if (lw == 1 && lh == 1) break; lw = std::max(1, lw / 2); lh = std::max(1, lh / 2);
        }
        t.mips = lvl; t.w = w; t.h = h; t.hash = hh;
      }
      t.hashFrame = frame;
    }
  }
  glBindTexture(GL_TEXTURE_2D, e->tex);
  bool mip = wantMips && e->mips > 1;
  GLenum minGL = mip ? ((minf == 1 || minf == 5) ? (minf == 5 ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                                                : ((minf == 2 || minf == 6) ? (minf == 6 ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR) : GL_LINEAR))
                     : ((minf >= 4) ? GL_LINEAR : GL_NEAREST);
  if (!mip && minf < 4 && minf != 0) minGL = GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minGL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode(mode0 & 3));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode((mode0 >> 2) & 3));
}

// ---------------------------------------------------------------- shader generation
static std::string konstSel(int sel, bool alpha) {
  static const char* fr[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
  char b[64];
  if (sel < 8) { snprintf(b, sizeof b, alpha ? "%s" : "vec3(%s)", fr[sel]); return b; }
  if (!alpha && sel >= 12 && sel < 16) { snprintf(b, sizeof b, "u_k[%d].rgb", sel - 12); return b; }
  if (sel >= 16) { int k = (sel - 16) & 3, c = (sel - 16) >> 2; const char* ch = "rgba"; snprintf(b, sizeof b, alpha ? "u_k[%d].%c" : "vec3(u_k[%d].%c)", k, ch[c & 3]); return b; }
  return alpha ? "0.0" : "vec3(0.0)";
}

static std::string buildFragment(Machine& m, int nTev, int nTexGen, int& usedMaps) {
  usedMaps = 0;
  std::string s = "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";
  s += "varying vec4 v_c0; varying vec4 v_c1;\n";
  for (int i = 0; i < std::max(1, nTexGen); i++) s += "varying vec3 v_t" + std::to_string(i) + ";\n";
  for (int i = 0; i < 8; i++) s += "uniform sampler2D s" + std::to_string(i) + ";\n";
  s += "uniform vec4 u_r[4]; uniform vec4 u_k[4]; uniform vec2 u_aref;\n";
  s += "vec2 pj(vec3 t){ return t.xy / (abs(t.z) < 1e-6 ? 1.0 : t.z); }\n";
  s += "void main(){\n vec4 r0=u_r[0], r1=u_r[1], r2=u_r[2], r3=u_r[3];\n";
  const char* rc = "rgba";
  auto swz = [&](int tbl) { std::string z = "."; u32 a = m.bpRegs[0xF6 + 2 * tbl], b = m.bpRegs[0xF7 + 2 * tbl];
    z += rc[a & 3]; z += rc[(a >> 2) & 3]; z += rc[b & 3]; z += rc[(b >> 2) & 3]; return z; };
  for (int st = 0; st < nTev; st++) {
    u32 cr = m.bpRegs[0xC0 + 2 * st], ar = m.bpRegs[0xC1 + 2 * st];
    u32 ord = m.bpRegs[0x28 + st / 2]; int map, coord, chan; bool en;
    if (st & 1) { map = (ord >> 12) & 7; coord = (ord >> 15) & 7; en = (ord >> 18) & 1; chan = (ord >> 19) & 7; }
    else { map = ord & 7; coord = (ord >> 3) & 7; en = (ord >> 6) & 1; chan = (ord >> 7) & 7; }
    u32 ks = m.bpRegs[0xF6 + st / 2]; int kc = (st & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31; int ka = (st & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
    s += " {\n  vec4 tx = vec4(0.0); vec4 rs = vec4(0.0);\n";
    if (en) { usedMaps |= 1 << map; int cc = std::min(coord, std::max(0, nTexGen - 1));
      s += "  tx = texture2D(s" + std::to_string(map) + ", pj(v_t" + std::to_string(cc) + "))" + swz((ar >> 2) & 3) + ";\n"; }
    if (chan == 0) s += "  rs = v_c0" + swz(ar & 3) + ";\n"; else if (chan == 1) s += "  rs = v_c1" + swz(ar & 3) + ";\n";
    static const char* ci[16] = {"r0.rgb", "vec3(r0.a)", "r1.rgb", "vec3(r1.a)", "r2.rgb", "vec3(r2.a)", "r3.rgb", "vec3(r3.a)",
                                 "tx.rgb", "vec3(tx.a)", "rs.rgb", "vec3(rs.a)", "vec3(1.0)", "vec3(0.5)", "KC", "vec3(0.0)"};
    auto csel = [&](int v) { return v == 14 ? konstSel(kc, false) : std::string(ci[v]); };
    static const char* ai[8] = {"r0.a", "r1.a", "r2.a", "r3.a", "tx.a", "rs.a", "KA", "0.0"};
    auto asel = [&](int v) { return v == 6 ? konstSel(ka, true) : std::string(ai[v]); };
    std::string A = csel((cr >> 12) & 15), B = csel((cr >> 8) & 15), C = csel((cr >> 4) & 15), D = csel(cr & 15);
    int bias = (cr >> 16) & 3, sub = (cr >> 18) & 1, clampC = (cr >> 19) & 1, scl = (cr >> 20) & 3, dst = (cr >> 22) & 3;
    s += "  vec3 ca=" + A + ", cb=" + B + ", cc_=" + C + ", cd=" + D + ";\n  vec3 co;\n";
    if (bias == 3) {
      int mode = (scl << 1) | sub; std::string cond;
      if (mode >= 6) {
        const char* op = mode == 6 ? ">" : "==";
        s += std::string("  co = cd + vec3(ca.r") + op + "cb.r ? cc_.r : 0.0, ca.g" + op + "cb.g ? cc_.g : 0.0, ca.b" + op + "cb.b ? cc_.b : 0.0);\n";
      } else {
        const char* op = (mode & 1) ? "==" : ">";
        std::string x = "floor(ca.r*255.0+0.5)", y = "floor(cb.r*255.0+0.5)";
        if (mode >= 2) { x += "+256.0*floor(ca.g*255.0+0.5)"; y += "+256.0*floor(cb.g*255.0+0.5)"; }
        if (mode >= 4) { x += "+65536.0*floor(ca.b*255.0+0.5)"; y += "+65536.0*floor(cb.b*255.0+0.5)"; }
        s += "  co = cd + ((" + x + ")" + op + "(" + y + ") ? cc_ : vec3(0.0));\n";
      }
    } else {
      static const char* bs[3] = {"0.0", "0.5", "-0.5"}; static const char* sc[4] = {"1.0", "2.0", "4.0", "0.5"};
      s += std::string("  co = (cd ") + (sub ? "-" : "+") + " (ca*(1.0-cc_) + cb*cc_) + vec3(" + bs[bias] + ")) * " + sc[scl] + ";\n";
    }
    s += clampC ? "  co = clamp(co, 0.0, 1.0);\n" : "  co = clamp(co, -4.0, 4.0);\n";
    int aA = (ar >> 13) & 7, aB = (ar >> 10) & 7, aC = (ar >> 7) & 7, aD = (ar >> 4) & 7;
    int abias = (ar >> 16) & 3, asub = (ar >> 18) & 1, aclamp = (ar >> 19) & 1, ascl = (ar >> 20) & 3, adst = (ar >> 22) & 3;
    s += "  float aa=" + asel(aA) + ", ab=" + asel(aB) + ", ac=" + asel(aC) + ", ad=" + asel(aD) + ";\n  float ao;\n";
    if (abias == 3) {
      int mode = (ascl << 1) | asub; const char* op = (mode & 1) ? "==" : ">";
      s += std::string("  ao = ad + ((floor(aa*255.0+0.5)") + op + "floor(ab*255.0+0.5)) ? ac : 0.0);\n";
    } else {
      static const char* bs[3] = {"0.0", "0.5", "-0.5"}; static const char* sc[4] = {"1.0", "2.0", "4.0", "0.5"};
      s += std::string("  ao = (ad ") + (asub ? "-" : "+") + " (aa*(1.0-ac) + ab*ac) + " + bs[abias] + ") * " + sc[ascl] + ";\n";
    }
    s += aclamp ? "  ao = clamp(ao, 0.0, 1.0);\n" : "  ao = clamp(ao, -4.0, 4.0);\n";
    s += "  r" + std::to_string(dst) + ".rgb = co;\n  r" + std::to_string(adst) + ".a = ao;\n }\n";
  }
  // alpha test
  u32 ac = m.bpRegs[0xF3]; int c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, lg = (ac >> 22) & 3;
  if (!(c0 == 7 && c1 == 7 && lg == 0) && !(c0 == 7 && lg == 1) && !(c1 == 7 && lg == 0 && c0 == 7)) {
    static const char* cmp[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    auto term = [&](int c, int u) { std::string v = "floor(r0.a*255.0+0.5)", r = "u_aref." + std::string(u ? "y" : "x");
      if (c == 0) return std::string("false"); if (c == 7) return std::string("true");
      return "(" + v + cmp[c] + r + ")"; };
    std::string t0 = term(c0, 0), t1 = term(c1, 1), e;
    if (lg == 0) e = t0 + " && " + t1; else if (lg == 1) e = t0 + " || " + t1; else if (lg == 2) e = "(" + t0 + ") != (" + t1 + ")"; else e = "(" + t0 + ") == (" + t1 + ")";
    s += " if (!(" + e + ")) discard;\n";
  }
  s += " gl_FragColor = clamp(r0, 0.0, 1.0);\n}\n";
  return s;
}

static std::string buildVertex(int nTexGen) {
  std::string s = "attribute vec4 a_pos; attribute vec4 a_c0; attribute vec4 a_c1;\n";
  int n = std::max(1, nTexGen);
  for (int i = 0; i < n; i++) s += "attribute vec3 a_t" + std::to_string(i) + "; varying vec3 v_t" + std::to_string(i) + ";\n";
  s += "varying vec4 v_c0; varying vec4 v_c1;\nvoid main(){ gl_Position = a_pos; v_c0 = a_c0; v_c1 = a_c1;\n";
  for (int i = 0; i < n; i++) s += " v_t" + std::to_string(i) + " = a_t" + std::to_string(i) + ";\n";
  return s + "}\n";
}

// ---------------------------------------------------------------- drawing
struct OutV { float pos[4]; float c0[4], c1[4]; float t[5][3]; };

static void lightChannel(Machine& m, int ch, const GXVertex& v, const float* vpos, const float* vnrm, float* out) {
  u32 colCtl = m.xf[0x100E + ch], alCtl = m.xf[0x1010 + ch];
  u32 amb = m.xf[0x100A + ch], mat = m.xf[0x100C + ch];
  auto unpack = [](u32 c, float* o) { o[0] = (c >> 24) / 255.f; o[1] = ((c >> 16) & 255) / 255.f; o[2] = ((c >> 8) & 255) / 255.f; o[3] = (c & 255) / 255.f; };
  float vc[4] = {1, 1, 1, 1}; if (v.hasCol[ch]) memcpy(vc, v.col[ch], 16);
  float matc[4], ambc[4]; unpack(mat, matc); unpack(amb, ambc);
  float M[4], A[4];
  for (int k = 0; k < 3; k++) { M[k] = (colCtl & 1) ? vc[k] : matc[k]; A[k] = ((colCtl >> 6) & 1) ? vc[k] : ambc[k]; }
  M[3] = (alCtl & 1) ? vc[3] : matc[3]; A[3] = ((alCtl >> 6) & 1) ? vc[3] : ambc[3];
  float rgb[3] = {M[0], M[1], M[2]}, al = M[3];
  auto lightsFor = [&](u32 ctl, float* acc, bool alpha) {
    if (!((ctl >> 1) & 1)) return false;
    u32 mask = ((ctl >> 2) & 15) | (((ctl >> 11) & 15) << 4);
    int diff = (ctl >> 7) & 3; bool attnOn = (ctl >> 9) & 1, spot = (ctl >> 10) & 1;
    float sum[3] = {0, 0, 0};
    for (int l = 0; l < 8; l++) if (mask & (1 << l)) {
      u32 base = 0x600 + l * 16; u32 lc = m.xf[base + 3]; float lcol[4]; unpack(lc, lcol);
      float lp[3] = {xff(m, base + 10), xff(m, base + 11), xff(m, base + 12)}, ld[3] = {xff(m, base + 13), xff(m, base + 14), xff(m, base + 15)};
      float L[3] = {lp[0] - vpos[0], lp[1] - vpos[1], lp[2] - vpos[2]}; float dist = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
      float Ln[3] = {L[0], L[1], L[2]}; if (dist > 0) for (int k = 0; k < 3; k++) Ln[k] /= dist;
      float dif = 1.f;
      if (diff) { dif = vnrm[0] * Ln[0] + vnrm[1] * Ln[1] + vnrm[2] * Ln[2]; if (diff == 2 && dif < 0) dif = 0; }
      float att = 1.f;
      if (attnOn && spot) {
        float cs = Ln[0] * ld[0] + Ln[1] * ld[1] + Ln[2] * ld[2];
        float ca[3] = {xff(m, base + 4), xff(m, base + 5), xff(m, base + 6)}, da[3] = {xff(m, base + 7), xff(m, base + 8), xff(m, base + 9)};
        float num = std::max(0.f, ca[0] + ca[1] * cs + ca[2] * cs * cs), den = da[0] + da[1] * dist + da[2] * dist * dist;
        att = (cs < 0 || den == 0) ? 0.f : num / den;
      } else if (attnOn && !spot) att = 0.f;   // specular attenuation not implemented
      if (alpha) sum[0] += lcol[3] * dif * att; else for (int k = 0; k < 3; k++) sum[k] += lcol[k] * dif * att;
    }
    for (int k = 0; k < 3; k++) acc[k] = sum[k];
    return true;
  };
  float acc[3];
  if (lightsFor(colCtl, acc, false)) for (int k = 0; k < 3; k++) rgb[k] = M[k] * std::min(1.f, std::max(0.f, A[k] + acc[k]));
  if (lightsFor(alCtl, acc, true)) al = M[3] * std::min(1.f, std::max(0.f, A[3] + acc[0]));
  out[0] = rgb[0]; out[1] = rgb[1]; out[2] = rgb[2]; out[3] = al;
}

static int g_drawsThisFrame = 0;
void Renderer::draw(int prim, const GXVertex* vtx, int n) {
  Machine& mm = *m; Impl* D = d; g_drawsThisFrame++;
  if (skipNow) return;
  { static int mx = GENV("MAXDRAW") ? atoi(GENV("MAXDRAW")) : -1; static int mn = GENV("MINDRAW") ? atoi(GENV("MINDRAW")) : -1; if (mx >= 0 && g_drawsThisFrame > mx) return; if (mn >= 0 && g_drawsThisFrame < mn) return; }
  u32 gen = mm.bpRegs[0x00]; int nTexGen = gen & 15, nTev = ((gen >> 10) & 15) + 1;
  int nChan = (gen >> 4) & 7; (void)nChan;
  glBindFramebuffer(GL_FRAMEBUFFER, D->fbo);
  glViewport(0, 0, D->fboW, D->fboH);
  // --- transform
  float P[7]; for (int i = 0; i < 7; i++) P[i] = xff(mm, 0x1020 + i); bool ortho = mm.xf[0x1026] & 1;
  float wd = xff(mm, 0x101A), ht = xff(mm, 0x101B), zr = xff(mm, 0x101C), xo = xff(mm, 0x101D) - 342.f, yo = xff(mm, 0x101E) - 342.f, fz = xff(mm, 0x101F);
  u32 midxA = mm.xf[0x1018], midxB = mm.xf[0x1019];
  int ntc = std::min(nTexGen, 5);
  std::vector<OutV> ov(n);
  for (int i = 0; i < n; i++) {
    const GXVertex& v = vtx[i]; OutV& o = ov[i];
    int pm = v.hasPmi ? v.pmi : (midxA & 63);
    const u32* X = &mm.xf[pm * 4];
    float f[12]; for (int k = 0; k < 12; k++) { u32 b = X[k]; memcpy(&f[k], &b, 4); }
    float vp[3] = {f[0] * v.pos[0] + f[1] * v.pos[1] + f[2] * v.pos[2] + f[3], f[4] * v.pos[0] + f[5] * v.pos[1] + f[6] * v.pos[2] + f[7], f[8] * v.pos[0] + f[9] * v.pos[1] + f[10] * v.pos[2] + f[11]};
    float xc, yc, zc, wc;
    if (!ortho) { xc = P[0] * vp[0] + P[1] * vp[2]; yc = P[2] * vp[1] + P[3] * vp[2]; zc = P[4] * vp[2] + P[5]; wc = -vp[2]; }
    else { xc = P[0] * vp[0] + P[1]; yc = P[2] * vp[1] + P[3]; zc = P[4] * vp[2] + P[5]; wc = 1.f; }
    o.pos[0] = (xc * wd + (xo - EFB_W / 2.f) * wc) / (EFB_W / 2.f);
    o.pos[1] = -(yc * ht + (yo - EFB_H / 2.f) * wc) / (EFB_H / 2.f);
    o.pos[2] = 2.f * (zc * zr + fz * wc) / 16777215.f - wc;
    o.pos[3] = wc;
    // normal in view space
    float nrm[3] = {0, 0, 1};
    if (v.hasNrm) { const u32* NM = &mm.xf[0x400 + (pm & 31) * 3]; float nf[9]; for (int k = 0; k < 9; k++) { u32 b = NM[k]; memcpy(&nf[k], &b, 4); }
      for (int r = 0; r < 3; r++) nrm[r] = nf[r * 3] * v.nrm[0] + nf[r * 3 + 1] * v.nrm[1] + nf[r * 3 + 2] * v.nrm[2];
      float l = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]); if (l > 0) for (int k = 0; k < 3; k++) nrm[k] /= l; }
    // colours
    int nCol = mm.xf[0x1009] & 3;
    for (int c = 0; c < 2; c++) {
      float* dst = c ? o.c1 : o.c0;
      if (c < nCol) lightChannel(mm, c, v, vp, nrm, dst); else { dst[0] = dst[1] = dst[2] = dst[3] = (c == 0 ? 1.f : 0.f); }
    }
    // texgen
    for (int t = 0; t < ntc; t++) {
      u32 info = mm.xf[0x1040 + t]; int src = (info >> 7) & 31, type = (info >> 4) & 7; bool proj = (info >> 1) & 1, abc1 = (info >> 2) & 3;
      float in[4] = {0, 0, 1, 1};
      if (src == 0) { in[0] = v.pos[0]; in[1] = v.pos[1]; in[2] = v.pos[2]; }
      else if (src == 1) { in[0] = v.nrm[0]; in[1] = v.nrm[1]; in[2] = v.nrm[2]; }
      else if (src == 2) { in[0] = v.col[0][0]; in[1] = v.col[0][1]; in[2] = 1; }
      else if (src >= 5 && src < 13) { int k = src - 5; in[0] = v.tex[k][0]; in[1] = v.tex[k][1]; in[2] = abc1 ? 0.f : 1.f; }
      if (!(src >= 5 && src < 13) || abc1) { if (src == 0 || src == 1) in[2] = in[2]; }
      float s, tt, q = 1.f;
      if (type == 2 || type == 3) { const float* cc = type == 2 ? o.c0 : o.c1; s = cc[0]; tt = cc[1]; }
      else {
        int tm = t < 4 ? (midxA >> (6 + 6 * t)) & 63 : (midxB >> (6 * (t - 4))) & 63; if (v.hasTmi[t]) tm = v.tmi[t];
        const u32* TX = &mm.xf[tm * 4]; float g[12]; for (int k = 0; k < 12; k++) { u32 b = TX[k]; memcpy(&g[k], &b, 4); }
        s = g[0] * in[0] + g[1] * in[1] + g[2] * in[2] + g[3] * in[3]; tt = g[4] * in[0] + g[5] * in[1] + g[6] * in[2] + g[7] * in[3];
        if (proj) q = g[8] * in[0] + g[9] * in[1] + g[10] * in[2] + g[11] * in[3];
        if (mm.xf[0x1012] & 1) { u32 pi = mm.xf[0x1050 + t] & 63; const u32* PX = &mm.xf[0x500 + pi * 4]; float h[12]; for (int k = 0; k < 12; k++) { u32 b = PX[k]; memcpy(&h[k], &b, 4); }
          float is[3] = {s, tt, q}; s = h[0] * is[0] + h[1] * is[1] + h[2] * is[2] + h[3]; tt = h[4] * is[0] + h[5] * is[1] + h[6] * is[2] + h[7]; q = h[8] * is[0] + h[9] * is[1] + h[10] * is[2] + h[11]; }
      }
      o.t[t][0] = s; o.t[t][1] = tt; o.t[t][2] = q;
    }
  }
  // --- assemble primitives
  std::vector<OutV> tri; tri.reserve(n * 2);
  GLenum mode = GL_TRIANGLES;
  switch (prim) {
    case 0: for (int i = 0; i + 3 < n; i += 4) { tri.push_back(ov[i]); tri.push_back(ov[i + 1]); tri.push_back(ov[i + 2]); tri.push_back(ov[i]); tri.push_back(ov[i + 2]); tri.push_back(ov[i + 3]); } break;
    case 2: for (int i = 0; i + 2 < n; i += 3) { tri.push_back(ov[i]); tri.push_back(ov[i + 1]); tri.push_back(ov[i + 2]); } break;
    case 3: for (int i = 0; i + 2 < n; i++) { if (i & 1) { tri.push_back(ov[i + 1]); tri.push_back(ov[i]); tri.push_back(ov[i + 2]); } else { tri.push_back(ov[i]); tri.push_back(ov[i + 1]); tri.push_back(ov[i + 2]); } } break;
    case 4: for (int i = 1; i + 1 < n; i++) { tri.push_back(ov[0]); tri.push_back(ov[i]); tri.push_back(ov[i + 1]); } break;
    case 5: mode = GL_LINES; for (int i = 0; i + 1 < n; i += 2) { tri.push_back(ov[i]); tri.push_back(ov[i + 1]); } break;
    case 6: mode = GL_LINES; for (int i = 0; i + 1 < n; i++) { tri.push_back(ov[i]); tri.push_back(ov[i + 1]); } break;
    case 7: mode = GL_POINTS; tri = ov; break;
    default: return;
  }
  if (tri.empty()) return;
  static long dbgFrame = GENV("DRAWLOG") ? atol(GENV("DRAWLOG")) : -1;
  bool dbg = (long)frame == dbgFrame;
  if (dbg) {
    u32 zm_ = mm.bpRegs[0x40], cm_ = mm.bpRegs[0x41];
    fprintf(stderr, "DRAW prim=%d n=%d tev=%d ntex=%d zmode=%X blend=%X cull=%d ortho=%d vp=(%g,%g,%g,%g) P=(%g %g %g %g %g %g)\n", prim, n, nTev, nTexGen, zm_, cm_, (gen >> 14) & 3, (int)ortho, wd, ht, xo, yo, P[0], P[1], P[2], P[3], P[4], P[5]);
    { int vis = 0; float x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9; for (int i = 0; i < n; i++) { float w = ov[i].pos[3]; if (w > 0) { vis++; float sx = ov[i].pos[0] / w, sy = ov[i].pos[1] / w; x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy); } }
      fprintf(stderr, "   F3=%06X PE43=%06X genmode=%06X\n", mm.bpRegs[0xF3], mm.bpRegs[0x43], mm.bpRegs[0x00]);
      fprintf(stderr, "   SCISSOR %06X %06X off59=%06X\n", mm.bpRegs[0x20], mm.bpRegs[0x21], mm.bpRegs[0x59]);
      fprintf(stderr, "   ZREG zr=%g fz=%g P4=%g P5=%g zc0=%g\n", zr, fz, P[4], P[5], 0.f);
      fprintf(stderr, "   SUM vis=%d/%d ndc=[%g..%g, %g..%g] tex0=%06X\n", vis, n, x0, x1, y0, y1, mm.bpRegs[0x94]); }
    for (int i = 0; i < std::min(n, 4); i++) fprintf(stderr, "   v%d pos=(%g %g %g) clip=(%g %g %g %g) c0=(%g %g %g %g) t0=(%g %g %g)\n", i, vtx[i].pos[0], vtx[i].pos[1], vtx[i].pos[2], ov[i].pos[0], ov[i].pos[1], ov[i].pos[2], ov[i].pos[3], ov[i].c0[0], ov[i].c0[1], ov[i].c0[2], ov[i].c0[3], ov[i].t[0][0], ov[i].t[0][1], ov[i].t[0][2]);
    { int pm = vtx[0].hasPmi ? vtx[0].pmi : (midxA & 63); const u32* X = &mm.xf[pm * 4]; float f[12]; for (int k = 0; k < 12; k++) memcpy(&f[k], &X[k], 4); fprintf(stderr, "   pm=%d hasPmi=%d M=[%g %g %g %g | %g %g %g %g | %g %g %g %g]\n", pm, (int)vtx[0].hasPmi, f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11]); }
    for (int s = 0; s < nTev; s++) fprintf(stderr, "   tev%d color=%06X alpha=%06X order=%06X\n", s, mm.bpRegs[0xC0 + 2 * s], mm.bpRegs[0xC1 + 2 * s], mm.bpRegs[0x28 + s / 2]);
    for (int i = 0; i < 4; i++) { int sreg = 0x80 + i; fprintf(stderr, "   map%d img0=%06X addr=%06X mode0=%06X\n", i, mm.bpRegs[sreg + 8], mm.bpRegs[sreg + 0x14], mm.bpRegs[sreg]); }
    fprintf(stderr, "   tevreg prev=(%g %g %g %g) k0=(%g %g %g %g) chan0ctl=%X mat0=%08X xf1009=%X\n", mm.tevCol[0][0], mm.tevCol[0][1], mm.tevCol[0][2], mm.tevCol[0][3], mm.tevKonst[0][0], mm.tevKonst[0][1], mm.tevKonst[0][2], mm.tevKonst[0][3], mm.xf[0x100E], mm.xf[0x100C], mm.xf[0x1009]);
  }
  // --- state
  u32 zm = mm.bpRegs[0x40], cm = mm.bpRegs[0x41], sc0 = mm.bpRegs[0x20], sc1 = mm.bpRegs[0x21];
  glEnable(GL_SCISSOR_TEST);
  { int l = ((sc0 >> 12) & 0xFFF) - 342, t = (sc0 & 0xFFF) - 342, r = ((sc1 >> 12) & 0xFFF) - 342, b = (sc1 & 0xFFF) - 342;
    l = std::max(l, 0); t = std::max(t, 0); r = std::min(r + 1, (int)EFB_W); b = std::min(b + 1, (int)EFB_H);
    float S = D->sc; int x0 = (int)floorf(l * S), y0 = (int)floorf((EFB_H - b) * S), x1 = (int)ceilf(r * S), y1 = (int)ceilf((EFB_H - t) * S);
    glScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)); }
  if (GENV("NODEPTH")) zm &= ~1u;
  if (zm & 1) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_NEVER + ((zm >> 1) & 7)); } else glDisable(GL_DEPTH_TEST);
  glDepthMask((zm & 0x10) ? GL_TRUE : GL_FALSE);
  glColorMask((cm >> 3) & 1, (cm >> 3) & 1, (cm >> 3) & 1, (cm >> 4) & 1);
  static const GLenum sf[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  static const GLenum df[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  if (GENV("NOBLEND")) cm &= ~1u;
  if (cm & 1) { glEnable(GL_BLEND); glBlendFunc(sf[(cm >> 8) & 7], df[(cm >> 5) & 7]); glBlendEquation((cm >> 11) & 1 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD); }
  else glDisable(GL_BLEND);
  int cull = (gen >> 14) & 3;
  if (GENV("NOCULL") || cull == 0 || mode != GL_TRIANGLES) glDisable(GL_CULL_FACE);
  else { glEnable(GL_CULL_FACE); glFrontFace(GL_CW); glCullFace(cull == 1 ? GL_BACK : (cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK)); }
  // --- program
  std::string key; key.reserve(256);
  auto add = [&](u32 v) { key.append((const char*)&v, 4); };
  add(nTev); add(ntc); for (int i = 0; i < nTev; i++) { add(mm.bpRegs[0xC0 + 2 * i]); add(mm.bpRegs[0xC1 + 2 * i]); add(mm.bpRegs[0x28 + i / 2]); }
  for (int i = 0; i < 8; i++) add(mm.bpRegs[0xF6 + i]); add(mm.bpRegs[0xF3] & 0xFF0000 ? mm.bpRegs[0xF3] & 0xFFFF0000 : 0); add(mm.bpRegs[0xF3] >> 16);
  Prog* pr; auto it = D->progs.find(key);
  int usedMaps = 0;
  if (it == D->progs.end()) {
    Prog np; std::string fs = buildFragment(mm, nTev, ntc, usedMaps);
    np.p = link(buildVertex(ntc), fs);
    np.uR = glGetUniformLocation(np.p, "u_r"); np.uK = glGetUniformLocation(np.p, "u_k"); np.uAref = glGetUniformLocation(np.p, "u_aref");
    for (int i = 0; i < 8; i++) np.uS[i] = glGetUniformLocation(np.p, ("s" + std::to_string(i)).c_str());
    it = D->progs.emplace(key, np).first;
    D->progs[key + "m"].p = usedMaps;   // remember which maps the shader samples
  }
  pr = &it->second; usedMaps = (int)D->progs[key + "m"].p;
  glUseProgram(pr->p);
  glUniform4fv(pr->uR, 4, &mm.tevCol[0][0]); glUniform4fv(pr->uK, 4, &mm.tevKonst[0][0]);
  glUniform2f(pr->uAref, (float)(mm.bpRegs[0xF3] & 255), (float)((mm.bpRegs[0xF3] >> 8) & 255));
  for (int i = 0; i < 8; i++) if (usedMaps & (1 << i)) { bindMap(mm, D, frame, i); if (pr->uS[i] >= 0) glUniform1i(pr->uS[i], i); }
  // --- upload + draw
  glBindBuffer(GL_ARRAY_BUFFER, D->vbo);
  glBufferData(GL_ARRAY_BUFFER, tri.size() * sizeof(OutV), tri.data(), GL_STREAM_DRAW);
  const GLsizei stride = sizeof(OutV);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(OutV, pos));
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(OutV, c0));
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(OutV, c1));
  for (int t = 0; t < 5; t++) { if (t < std::max(1, ntc)) { glEnableVertexAttribArray(3 + t); glVertexAttribPointer(3 + t, 3, GL_FLOAT, GL_FALSE, stride, (void*)(offsetof(OutV, t) + t * 12)); } else glDisableVertexAttribArray(3 + t); }
  glDrawArrays(mode, 0, (GLsizei)tri.size());
}

// ---------------------------------------------------------------- EFB copy / present
void Renderer::efbCopy(u32 v) {
  Machine& mm = *m; Impl* D = d;
  bool toXfb = (v >> 14) & 1, clear = (v >> 11) & 1;
  u32 src = mm.bpRegs[0x49], sz = mm.bpRegs[0x4A], dst = mm.bpRegs[0x4B];
  int x = src & 0x3FF, y = (src >> 10) & 0x3FF, w = (sz & 0x3FF) + 1, h = ((sz >> 10) & 0x3FF) + 1;
  bool half = (v >> 9) & 1;
  if (GENV("DRAWLOG") && (long)frame == atol(GENV("DRAWLOG"))) fprintf(stderr, "EFBCOPY after draw %d toXfb=%d clear=%d v=%X src=%X sz=%X dst=%X zbuf=%X\n", g_drawsThisFrame, (int)toXfb, (int)clear, v, src, sz, dst, mm.bpRegs[0x51]);
  if (toXfb) {
    if (skipNow) { frame++; skippedFrames++; g_drawsThisFrame = 0; frameskipNext(); pollEvents(); }
    else { present(); frameskipNext(); }
  } else {
    texCopyHold = 120;   // the game renders to textures: skipping would leave them stale, so stay off for a while
    int fmt = ((v >> 4) & 7) | (((v >> 3) & 1) << 3);
    int ow = half ? std::max(1, w / 2) : w, oh = half ? std::max(1, h / 2) : h;
    u32 addr = (dst & 0xFFFFFF) << 5;
    TexEntry& e = D->efbTex[addr];
    if (!e.tex || e.w != ow || e.h != oh) { if (!e.tex) glGenTextures(1, &e.tex); glBindTexture(GL_TEXTURE_2D, e.tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); e.w = ow; e.h = oh; e.efb = true; e.mips = 1; }
    glBindFramebuffer(GL_FRAMEBUFFER, D->copyFbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.tex, 0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ow, oh);
    glUseProgram(D->copyProg); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, D->fboTex);
    glUniform1i(glGetUniformLocation(D->copyProg, "s0"), 0);
    int mode = (fmt == 0 || fmt == 1 || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 11 || fmt == 12) ? 1 : (fmt == 2 || fmt == 3) ? 2 : (fmt == 7) ? 3 : (fmt == 4) ? 4 : 0;
    glUniform1i(glGetUniformLocation(D->copyProg, "u_mode"), mode);
    // quad covering the source rectangle: v_uv = a_pos*0.5+0.5 over the whole viewport -> remap via uniform-free trick: use scissorless sub-rect uvs
    float u0 = x / (float)EFB_W, u1 = (x + w) / (float)EFB_W, v0 = 1.f - (y + h) / (float)EFB_H, v1 = 1.f - y / (float)EFB_H;
    // flip vertically so row 0 of the texture is the top of the EFB image
    float q[16] = {-1, -1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1, 1, 0, 0}; (void)q; (void)u0; (void)u1; (void)v0; (void)v1;
    // draw with a remapped texture-matrix-free approach: temporarily set texture coords via vertex positions of a custom quad
    static GLuint qbuf = 0; if (!qbuf) glGenBuffers(1, &qbuf);
    float pos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, qbuf); glBufferData(GL_ARRAY_BUFFER, sizeof pos, pos, GL_STREAM_DRAW);
    // v_uv = a_pos*0.5+0.5 spans [0,1]; to sample a sub-rect we adjust the viewport so the full-EFB texture is mapped and the rect is cropped
    glViewport(-(int)(x * (ow / (float)w)), -(int)((EFB_H - (y + h)) * (oh / (float)h)) , (int)(EFB_W * (ow / (float)w)), (int)(EFB_H * (oh / (float)h)));
    glEnable(GL_SCISSOR_TEST); glScissor(0, 0, ow, oh);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for (int i = 1; i < 8; i++) glDisableVertexAttribArray(i);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, D->fbo);
  }
  if (clear) {
    u32 ar = mm.bpRegs[0x4F], gb = mm.bpRegs[0x50], z = mm.bpRegs[0x51];
    glBindFramebuffer(GL_FRAMEBUFFER, D->fbo); glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1); glClearColor((ar & 255) / 255.f, ((gb >> 8) & 255) / 255.f, (gb & 255) / 255.f, ((ar >> 8) & 255) / 255.f);
    glDepthMask(GL_TRUE); glClearDepthf((z & 0xFFFFFF) / 16777215.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
}

void Renderer::frameskipNext() {
  using clk = std::chrono::steady_clock; static clk::time_point last = clk::now();
  clk::time_point now = clk::now(); double ms = std::chrono::duration<double, std::milli>(now - last).count(); last = now;
  if (ms > 200) ms = 200; emaMs = emaMs * 0.8 + ms * 0.2;
  if (texCopyHold > 0) texCopyHold--;
  if (fsMode == 0 || texCopyHold > 0) { skipNow = false; return; }
  int level = fsMode;
  if (fsMode < 0) {   // auto: 50 Hz target (20 ms); step up when clearly behind, step down when there is headroom
    if (emaMs > 38) autoLevel = 2; else if (emaMs > 24 && autoLevel < 1) autoLevel = 1; else if (emaMs < 17 && autoLevel > 0) autoLevel--;
    level = autoLevel;
  }
  skipNow = level > 0 && (frame % (level + 1)) != 0;
}

void Renderer::present() {
  Impl* D = d; frame++;
  if (GENV("DRAWSTAT")) { fprintf(stderr, "frame %llu: %d draws\n", (unsigned long long)frame, g_drawsThisFrame); } g_drawsThisFrame = 0;
  if (const char* shotSpec = GENV("SHOT")) {   // "frame:path,frame:path,..."
    std::string str = shotSpec; size_t p = 0;
    while (p < str.size()) { size_t q = str.find(',', p); if (q == std::string::npos) q = str.size(); std::string it = str.substr(p, q - p); size_t c = it.find(':');
      if (c != std::string::npos && strtoull(it.c_str(), 0, 10) == frame) { glBindFramebuffer(GL_FRAMEBUFFER, D->fbo); saveShot(it.c_str() + c + 1); } p = q + 1; }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  int w, h; SDL_GL_GetDrawableSize(D->win, &w, &h);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glColorMask(1, 1, 1, 1);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  { int vw = w, vh = h; if (w * 3 > h * 4) vw = h * 4 / 3; else vh = w * 3 / 4; glViewport((w - vw) / 2, (h - vh) / 2, vw, vh); }
  int fm = D->highp ? filterMode : 0; bool epx = fm >= 3, scan = fm == 1 || fm == 4, sharp = fm >= 2;
  static GLuint qbuf = 0; if (!qbuf) glGenBuffers(1, &qbuf);
  float pos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
  glBindBuffer(GL_ARRAY_BUFFER, qbuf); glBufferData(GL_ARRAY_BUFFER, sizeof pos, pos, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  for (int i = 1; i < 8; i++) glDisableVertexAttribArray(i);
  GLuint srcTex = D->fboTex; float srcW = D->fboW, srcH = D->fboH;
  if (epx) {   // pass 1: Scale2x into the 2x texture
    glBindFramebuffer(GL_FRAMEBUFFER, D->epxFbo); glViewport(0, 0, D->fboW * 2, D->fboH * 2);
    glUseProgram(D->epxProg); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, D->fboTex);
    glUniform1i(glGetUniformLocation(D->epxProg, "s0"), 0); glUniform2f(glGetUniformLocation(D->epxProg, "u_src"), (float)D->fboW, (float)D->fboH);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); srcTex = D->epxTex; srcW *= 2; srcH *= 2;
    int vw = w, vh = h; if (w * 3 > h * 4) vw = h * 4 / 3; else vh = w * 3 / 4; glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
  }
  int vh2 = (w * 3 > h * 4) ? h : w * 3 / 4;
  glUseProgram(D->presentProg); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, srcTex);
  glUniform1i(glGetUniformLocation(D->presentProg, "s0"), 0);
  glUniform2f(glGetUniformLocation(D->presentProg, "u_size"), srcW, srcH);
  glUniform1f(glGetUniformLocation(D->presentProg, "u_lines"), (float)D->fboH);
  glUniform1f(glGetUniformLocation(D->presentProg, "u_scale"), std::max(1.0f, vh2 / srcH));
  glUniform1f(glGetUniformLocation(D->presentProg, "u_sharp"), sharp ? 1.0f : 0.0f);
  glUniform1f(glGetUniformLocation(D->presentProg, "u_scan"), scan ? 1.0f : 0.0f);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (g_touchUi) drawTouchOverlay(w, h);
  if (const char* ws = getenv("WINSHOT")) { if (frame == (getenv("WINFRAME") ? strtoull(getenv("WINFRAME"), 0, 10) : 600)) { std::vector<u8> px((size_t)w * h * 4); glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = fopen(ws, "wb"); if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f); fclose(f); } } }
  SDL_GL_SwapWindow(D->win);
  glBindFramebuffer(GL_FRAMEBUFFER, D->fbo);
  pollEvents();
}

bool Renderer::saveShot(const char* path) {
  int W = d->fboW, H = d->fboH;
  std::vector<u8> px((size_t)W * H * 4);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  FILE* f = fopen(path, "wb"); if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) fwrite(&px[((size_t)y * W + x) * 4], 1, 3, f);
  fclose(f); fprintf(stderr, "screenshot -> %s (frame %llu)\n", path, (unsigned long long)frame); return true;
}


// ---------------- on-screen touch controls (Android, or TOUCH=1 on desktop) ----------------
// Layout in normalised window coordinates (x right, y down; radius relative to window height).
struct TBtn { float x, y, r; u16 bit; };
static const TBtn kBtns[] = {
  {0.90f, 0.70f, 0.085f, 0x0001}, {0.79f, 0.80f, 0.060f, 0x0002}, {0.93f, 0.46f, 0.060f, 0x0004}, {0.80f, 0.56f, 0.060f, 0x0008},   // A B X Y
  {0.50f, 0.92f, 0.045f, 0x0010}, {0.08f, 0.10f, 0.055f, 0x4000}, {0.92f, 0.10f, 0.055f, 0x2000}, {0.80f, 0.10f, 0.055f, 0x1000},   // Start L R Z
};
static const float kStickX = 0.14f, kStickY = 0.70f, kStickR = 0.13f;

struct Finger { SDL_FingerID id; float x, y; };
static std::vector<Finger> g_fingers;
static u16 g_touchMask = 0;
static void touchRecompute(Machine* m, int w, int h) {
  u16 bits = 0; bool stick = false; float aspect = (float)w / (float)h;
  for (auto& f : g_fingers) {
    float dx = (f.x - kStickX) * aspect, dy = f.y - kStickY;   // in units of window height
    if (f.x < 0.40f) {
      float lim = kStickR * 1.6f; float len = sqrtf(dx * dx + dy * dy); if (len > lim) { dx *= lim / len; dy *= lim / len; }
      float nx = dx / kStickR, ny = dy / kStickR; if (nx > 1) nx = 1; if (nx < -1) nx = -1; if (ny > 1) ny = 1; if (ny < -1) ny = -1;
      m->padStick[0] = (u8)(128 + nx * 100); m->padStick[1] = (u8)(128 - ny * 100); stick = true;
      if (nx > 0.55f) bits |= 0x0200; if (nx < -0.55f) bits |= 0x0100; if (ny > 0.55f) bits |= 0x0400; if (ny < -0.55f) bits |= 0x0800;
      continue;
    }
    for (auto& b : kBtns) { float bx = (f.x - b.x) * aspect, by = f.y - b.y; if (bx * bx + by * by < b.r * b.r * 1.7f) bits |= b.bit; }
  }
  if (!stick && !g_fingers.empty()) { m->padStick[0] = m->padStick[1] = 128; }
  if (!stick && g_touchMask) { /* released: recentre */ bool any = false; for (auto& f : g_fingers) if (f.x < 0.4f) any = true; if (!any) m->padStick[0] = m->padStick[1] = 128; }
  m->padBtn = (m->padBtn & ~(u16)0x0F1F & ~(u16)0x7F00 & ~(u16)0x1000) | bits; g_touchMask = bits | 1;
}
static GLuint g_ovProg = 0;
static void drawTouchOverlay(int w, int h) {
  if (!g_ovProg) {
    g_ovProg = link("attribute vec2 a_pos; void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); }", "precision mediump float; uniform vec4 u_c; void main(){ gl_FragColor = u_c; }");
  }
  glViewport(0, 0, w, h); glUseProgram(g_ovProg); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  GLint uc = glGetUniformLocation(g_ovProg, "u_c"); float aspect = (float)w / h;
  auto disc = [&](float cx, float cy, float r, float a) {   // filled disc as a triangle fan, coordinates normalised, r relative to height
    float v[2 * 26]; v[0] = cx * 2 - 1; v[1] = 1 - cy * 2;
    for (int i = 0; i <= 24; i++) { float t = i * 6.2831853f / 24; v[2 + 2 * i] = v[0] + cosf(t) * r * 2 / aspect; v[3 + 2 * i] = v[1] + sinf(t) * r * 2; }
    glUniform4f(uc, 1, 1, 1, a); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v); glEnableVertexAttribArray(0); glDrawArrays(GL_TRIANGLE_FAN, 0, 26);
  };
  for (int i = 1; i < 8; i++) glDisableVertexAttribArray(i);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  disc(kStickX, kStickY, kStickR, 0.12f);
  for (auto& f : g_fingers) if (f.x < 0.40f) disc(f.x, f.y, 0.04f, 0.3f);
  for (auto& b : kBtns) disc(b.x, b.y, b.r, (g_touchMask & b.bit) ? 0.45f : 0.16f);
  glDisable(GL_BLEND);
}
static SDL_GameController* g_pad = nullptr;
void Renderer::pollEvents() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) m->cpu.halt("window closed");
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F2) { filterMode = (filterMode + 1) % kFilters; static const char* nm[] = {"off", "scanlines", "sharp", "scale2x", "scale2x+scanlines"}; fprintf(stderr, "filter: %s\n", nm[filterMode]); }
    if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
      g_touchUi = true; int w, h; SDL_GL_GetDrawableSize(d->win, &w, &h);
      auto it = g_fingers.begin(); for (; it != g_fingers.end(); ++it) if (it->id == e.tfinger.fingerId) break;
      if (e.type == SDL_FINGERUP) { if (it != g_fingers.end()) g_fingers.erase(it); }
      else if (it != g_fingers.end()) { it->x = e.tfinger.x; it->y = e.tfinger.y; } else g_fingers.push_back({e.tfinger.fingerId, e.tfinger.x, e.tfinger.y});
      touchRecompute(m, w, h);
      if (g_fingers.empty()) m->padStick[0] = m->padStick[1] = 128;
    }
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
      bool dn = e.type == SDL_KEYDOWN; u16 bit = 0; if (dn && e.key.keysym.sym == SDLK_ESCAPE) m->cpu.halt("quit");
      switch (e.key.keysym.sym) {
        case SDLK_z: bit = 0x0001; break; case SDLK_x: bit = 0x0002; break; case SDLK_c: bit = 0x0004; break; case SDLK_v: bit = 0x0008; break;
        case SDLK_RETURN: bit = 0x0010; break; case SDLK_LEFT: bit = 0x0100; break; case SDLK_RIGHT: bit = 0x0200; break;
        case SDLK_DOWN: bit = 0x0400; break; case SDLK_UP: bit = 0x0800; break; case SDLK_a: bit = 0x1000; break;
        case SDLK_s: bit = 0x2000; break; case SDLK_d: bit = 0x4000; break; default: break; }
      if (bit) { if (dn) m->padBtn |= bit; else m->padBtn &= ~bit; }
      // arrow keys also drive the analog stick (fighting moves use it)
      const Uint8* ks = SDL_GetKeyboardState(nullptr);
      m->padStick[0] = ks[SDL_SCANCODE_RIGHT] ? 228 : ks[SDL_SCANCODE_LEFT] ? 28 : 128; m->padStick[1] = ks[SDL_SCANCODE_UP] ? 228 : ks[SDL_SCANCODE_DOWN] ? 28 : 128;
    }
    if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
      bool dn = e.type == SDL_CONTROLLERBUTTONDOWN; u16 bit = 0;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_B: bit = 0x0001; break; case SDL_CONTROLLER_BUTTON_A: bit = 0x0002; break; case SDL_CONTROLLER_BUTTON_Y: bit = 0x0004; break; case SDL_CONTROLLER_BUTTON_X: bit = 0x0008; break;
        case SDL_CONTROLLER_BUTTON_START: bit = 0x0010; break; case SDL_CONTROLLER_BUTTON_DPAD_LEFT: bit = 0x0100; break; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: bit = 0x0200; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: bit = 0x0400; break; case SDL_CONTROLLER_BUTTON_DPAD_UP: bit = 0x0800; break; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: bit = 0x2000; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: bit = 0x4000; break; case SDL_CONTROLLER_BUTTON_BACK: bit = 0x1000; break; default: break; }
      if (bit) { if (dn) m->padBtn |= bit; else m->padBtn &= ~bit; }
    }
    if (e.type == SDL_CONTROLLERAXISMOTION) {
      int v = e.caxis.value; u8 b = (u8)(128 + v / 256); if (v < -32000) b = 0;
      switch (e.caxis.axis) { case SDL_CONTROLLER_AXIS_LEFTX: m->padStick[0] = b; break; case SDL_CONTROLLER_AXIS_LEFTY: m->padStick[1] = (u8)(255 - b); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: m->padStick[2] = b; break; case SDL_CONTROLLER_AXIS_RIGHTY: m->padStick[3] = (u8)(255 - b); break; default: break; }
    }
  }
}
