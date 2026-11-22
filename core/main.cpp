#include <csignal>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include "jit_rt.h"
#include <algorithm>
#include "gc.h"
#include "gl.h"
#include <cstdlib>

#include <map>
#include <mutex>
#include <deque>
#include <thread>
#include <chrono>
#include <SDL2/SDL.h>
std::map<u32,int> g_prof;
u64 g_oph[64*1024];
// ---- audio sink: AI DMA output -> SDL (32 kHz stereo s16); also paces the emulation when RT=1 ----
static std::mutex g_amx; static std::deque<s16> g_aq;
static void audioCb(void*, Uint8* out, int len) {
  std::lock_guard<std::mutex> lk(g_amx); s16* o = (s16*)out; int n = len / 2;
  for (int i = 0; i < n; i++) { if (g_aq.empty()) { o[i] = 0; continue; } o[i] = g_aq.front(); g_aq.pop_front(); }
}
static SDL_AudioDeviceID g_adev = 0;
static void audioInit() {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
  SDL_AudioSpec w{}, h{}; w.freq = 32000; w.format = AUDIO_S16SYS; w.channels = 2; w.samples = 1024; w.callback = audioCb;
  g_adev = SDL_OpenAudioDevice(nullptr, 0, &w, &h, 0); if (g_adev) SDL_PauseAudioDevice(g_adev, 0);
}
static void audioPump(Machine* m, bool keep) {
  static size_t pos = 0; if (!g_adev) return; if (!keep) pos = 0; if (m->audioOut.size() <= pos) return;
  { std::lock_guard<std::mutex> lk(g_amx); g_aq.insert(g_aq.end(), m->audioOut.begin() + pos, m->audioOut.end()); if (g_aq.size() > 32000 * 2) g_aq.erase(g_aq.begin(), g_aq.begin() + (g_aq.size() - 32000 * 2)); }
  if (keep) pos = m->audioOut.size(); else m->audioOut.clear();
}
static void audioThrottle() {   // keep ~120 ms queued: the audio clock is the emulation's real-time reference
  for (;;) { size_t n; { std::lock_guard<std::mutex> lk(g_amx); n = g_aq.size(); } if (n < 32000 * 2 * 120 / 1000) break; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
JitBackend* (*g_backendFactory)() = nullptr;
void flushMemCard();
int main(int argc, char** argv) {
#ifdef __ANDROID__
  { const char* ip = SDL_AndroidGetInternalStoragePath();
    if (ip) { static std::string lp = std::string(ip) + "/last.log"; if (!freopen(lp.c_str(), "w", stderr)) {} setvbuf(stderr, nullptr, _IONBF, 0);
      static auto crash = [](int sig, siginfo_t* si, void*) { fprintf(stderr, "\nCRASH: signal %d at address %p\n", sig, si ? si->si_addr : nullptr); _exit(128 + sig); };
      struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = crash; sa.sa_flags = SA_SIGINFO; for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
      fprintf(stderr, "DRA2 start, %d args:", argc); for (int i = 0; i < argc; i++) fprintf(stderr, " %s", argv[i]); fputc(10, stderr); } }
  { const char* ip = SDL_AndroidGetInternalStoragePath(); if (ip && !getenv("DRA2_CARD")) { static std::string cp = std::string(ip) + "/card_a.raw"; setenv("DRA2_CARD", cp.c_str(), 1); } }
#endif
  if (argc < 2) { fprintf(stderr, "usage: %s game.iso [max_million_instr] [-t] [-fs off|N|auto]\n", argv[0]); return 1; }
  Machine* m = new Machine();
  m->bus.m = m; m->cpu.m = m; m->cpu.bus = &m->bus;
  for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "-t")) m->trace = true;
  if (!getenv("NOJIT")) { JitBackend* be = g_backendFactory ? g_backendFactory() : makeNativeBackend(); if (be && !getenv("INTERP")) { g_jit = new JitRT(); g_jit->be = be; fprintf(stderr, "JIT enabled\n"); } }
  if (!m->loadIso(argv[1]) || !m->boot()) { fprintf(stderr, "load/boot failed (errno %d: %s)\n", errno, strerror(errno)); return 2; }
  if (!getenv("NOGFX")) { m->rd = new Renderer(); if (!m->rd->init(m, !getenv("HIDDEN"))) { fprintf(stderr, "renderer init failed: %s\n", SDL_GetError()); m->rd = nullptr; } }
  if (m->rd) { const char* fs = getenv("FRAMESKIP"); for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "-fs")) fs = argv[i + 1];
    if (fs) m->rd->fsMode = !strcmp(fs, "auto") ? -1 : atoi(fs); }
  bool rt = !getenv("NORT") && !getenv("HIDDEN") && !getenv("NOGFX"); if (!getenv("NOAUDIO")) audioInit();
  u64 maxI = (argc > 2 && argv[2][0] != 0x2d ? strtoull(argv[2], 0, 10) : 100000000ull) * 1000000ull;
  u64 nextRep = 0;
  while (!m->cpu.halted && m->cpu.icount < maxI) {
    m->cpu.step(4096); m->tick(); audioPump(m, getenv("WAVOUT") != nullptr); if (rt && g_adev) audioThrottle();
    { static u64 pa = getenv("PROFA") ? strtoull(getenv("PROFA"),0,10)*1000000ull : 0, pb = getenv("PROFB") ? strtoull(getenv("PROFB"),0,10)*1000000ull : 0; if (pa && m->cpu.icount >= pa && m->cpu.icount < pb) { for (int k = 0; k < 4; k++) { /*sample*/ } extern std::map<u32,int> g_prof; g_prof[m->cpu.pc >> 4]++; } }
    if (getenv("STATUS") && m->cpu.icount >= nextRep) { nextRep += 100000000; fprintf(stderr, "[status] %4lluM pc=%08X draws=%llu finish=%llu efbcopy=%llu verts=%llu\n", (unsigned long long)(m->cpu.icount / 1000000), m->cpu.pc, (unsigned long long)m->gxStats.draws, (unsigned long long)m->gxStats.finish, (unsigned long long)m->gxStats.efbCopies, (unsigned long long)m->gxStats.verts); }
  }
#ifdef OPHIST
  { std::vector<std::pair<u64,int>> v; for (int i = 0; i < 64*1024; i++) if (g_oph[i]) v.push_back({g_oph[i], i}); std::sort(v.rbegin(), v.rend()); for (int i = 0; i < 30 && i < (int)v.size(); i++) fprintf(stderr, "OPH op=%d xo=%d n=%llu\n", v[i].second/1024, v[i].second%1024, (unsigned long long)v[i].first); }
#endif
  if (m->rd && m->rd->skippedFrames) fprintf(stderr, "frameskip: %llu frames skipped of %llu\n", (unsigned long long)m->rd->skippedFrames, (unsigned long long)m->rd->frame);
  { extern u64 g_skipped; fprintf(stderr, "executed=%llu skipped=%llu\n", (unsigned long long)(m->cpu.icount - g_skipped), (unsigned long long)g_skipped); }
  if (!g_prof.empty()) { std::vector<std::pair<int,u32>> v; for (auto& kv : g_prof) v.push_back({kv.second, kv.first}); std::sort(v.rbegin(), v.rend()); for (int i = 0; i < 25 && i < (int)v.size(); i++) fprintf(stderr, "PROF %08X %d\n", v[i].second << 4, v[i].first); }
  if (const char* wp = getenv("WAVOUT")) { FILE* f = fopen(wp, "wb"); if (f) { u32 n = (u32)m->audioOut.size() * 2, rate = 32000, br = rate * 4; u16 ch = 2, ba = 4, bps = 16, fmt = 1; u32 sz16 = 16, riff = 36 + n;
      fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sz16, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(m->audioOut.data(), 2, m->audioOut.size(), f); fclose(f); } }
  flushMemCard();
  CPU& c = m->cpu;
  fprintf(stderr, "\n== stopped after %llu instr: %s\n", (unsigned long long)c.icount, c.halted ? c.haltReason.c_str() : "limit");
  fprintf(stderr, "pc=%08X lr=%08X ctr=%08X msr=%08X cr=%08X xer=%08X\n", c.pc, c.lr, c.ctr, c.msr, c.cr, c.xer);
  for (int i = 0; i < 32; i += 4) fprintf(stderr, "r%02d %08X %08X %08X %08X\n", i, c.gpr[i], c.gpr[i+1], c.gpr[i+2], c.gpr[i+3]);
  fprintf(stderr, "last pcs:"); for (int i = 16; i > 0; i--) fprintf(stderr, " %08X", c.pcHist[(c.pcHistPos - i) & 63]); fputc('\n', stderr);
  if (getenv("DUMP")) { u32 a = strtoul(getenv("DUMP"), 0, 16); for (int i = 0; i < 12; i++) fprintf(stderr, "%08X: %08X\n", a + i * 4, m->bus.r32(a + i * 4)); }
  { fprintf(stderr, "stack:"); u32 sp = c.gpr[1]; for (int i = 0; i < 12 && sp > 0x80000000u && sp < 0x81800000u; i++) { u32 nx = m->bus.r32(sp); fprintf(stderr, " %08X", m->bus.r32(nx + 4)); sp = nx; } fputc('\n', stderr); }
  for (int r = 3; r <= 10; r++) { u32 p = c.gpr[r]; if (p > 0x80000000u && p < 0x81800000u) { char b[80] = {0}; for (int i = 0; i < 79; i++) { u8 ch = m->bus.r8(p + i); if (!ch) break; b[i] = (ch >= 32 && ch < 127) ? ch : '.'; } fprintf(stderr, "r%d -> \"%s\"\n", r, b); } }
  if (getenv("THREADS")) { Bus& b = m->bus; u32 t = b.r32(0x800000DC); fprintf(stderr, "OS threads (cur=%08X):\n", b.r32(0x800000E4));
    for (int i = 0; i < 16 && t; i++, t = b.r32(t + 0x2F8)) {
      u32 sp = b.r32(t + 4), lr = b.r32(t + 0x84), srr0 = b.r32(t + 0x198);
      fprintf(stderr, "  thr %08X state=%u pri=%d queue=%08X srr0=%08X lr=%08X sp=%08X | bt:", t, b.r16(t + 0x2C8), (int)b.r32(t + 0x2D0), b.r32(t + 0x2DC), srr0, lr, sp);
      u32 f = sp; for (int k = 0; k < 8 && f > 0x80000000u && f < 0x81800000u; k++) { u32 nx = b.r32(f); fprintf(stderr, " %08X", b.r32(nx + 4)); f = nx; }
      fputc('\n', stderr); } }
  fprintf(stderr, "gx: fifo=%zu consumed=%zu draws=%llu verts=%llu finish=%llu token=%llu efbcopy=%llu cp=%llu xf=%llu idxXF=%llu calldl=%llu\n", m->gxfifo.size(), m->gxPos, (unsigned long long)m->gxStats.draws, (unsigned long long)m->gxStats.verts, (unsigned long long)m->gxStats.finish, (unsigned long long)m->gxStats.token, (unsigned long long)m->gxStats.efbCopies, (unsigned long long)m->gxStats.cpLoads, (unsigned long long)m->gxStats.xfLoads, (unsigned long long)m->gxStats.indexedXf, (unsigned long long)m->gxStats.callDL);
  if (const char* dr = getenv("DUMPRAM")) { FILE* f = fopen(dr, "wb"); if (f) { fwrite(m->bus.ram, 1, 24u << 20, f); fclose(f); } }
  fprintf(stderr, "ints: cause=%X mask=%X di0=%04X di1=%04X di2=%04X di3=%04X msr=%X dec=%X viCount=%llu\n", m->piIntCause, m->piIntMask, m->viReg[0x18], m->viReg[0x1A], m->viReg[0x1C], m->viReg[0x1E], m->cpu.msr, m->cpu.dec, (unsigned long long)m->viCount);
  return 0;
}
