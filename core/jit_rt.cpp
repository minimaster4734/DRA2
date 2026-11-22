#include "jit_rt.h"
#include <cstdio>
#include <cstdlib>
#if defined(__arm__)
#include <sys/mman.h>
#endif

JitRT* g_jit = nullptr;

void JitRT::invalidateAll() {
  for (auto& e : table) e.pc = 1;
  if (fast) for (u32 i = 0; i < 65536; i++) fast[2 * i] = 1;
  std::fill(codePage.begin(), codePage.end(), 0);
  pending.clear();
  if (be) be->flush();
  if (cpu) cpu->jitLimit = 0;   // running chains bail out to the dispatcher at their next exit
  flushes++;
}
void JitRT::invalidateRange(u32 pa, u32 len) {
  pa &= 0x1FFFFFFF; if (pa >= RAM_SIZE) return;
  u32 last = (pa + (len ? len - 1 : 0)) >> 12; if (last >= codePage.size()) last = codePage.size() - 1;
  for (u32 p = pa >> 12; p <= last; p++) if (codePage[p]) { invalidateAll(); return; }
}

bool JitRT::isSpin(u32 pc) const { for (int i = 0; i < cpu->nspins; i++) if (cpu->spins[i].pc == pc) return true; return false; }

JitRT::Entry& JitRT::lookup(CPU* c, u32 pc) {
  cpu = c;
  if (!fast) { fast = (u32*)dra2_aligned_alloc(4096, 65536 * 8); for (u32 i = 0; i < 65536; i++) { fast[2 * i] = 1; fast[2 * i + 1] = 0; } c->jitFast = be->mapFast(fast, 65536 * 8); }
  Entry& e = table[(pc >> 2) & 0xFFFF];
  if (e.pc == pc) return e;
  u32 pa = pc & 0x1FFFFFFF;
  int maxw = (4096 - (pa & 4095)) / 4; if (maxw > 64) maxw = 64;
  u32 words[64];
  for (int i = 0; i < maxw; i++) { u32 v; memcpy(&v, c->bus->ram + pa + 4 * i, 4); words[i] = __builtin_bswap32(v); }
  std::vector<u32> code; code.reserve(512);
  std::vector<JitLink> links; jitTranslate(code, be->helpers(), pc, words, maxw, &links);
  u8* entry = be->install(code.data(), code.size());
  if (!entry) { invalidateAll(); entry = be->install(code.data(), code.size()); }
  Entry& e2 = table[(pc >> 2) & 0xFFFF];
  e2.pc = pc; e2.code = entry; codePage[pa >> 12] = 1; translated++;
  if (!isSpin(pc)) { u32 i = (pc >> 2) & 0xFFFF; fast[2 * i] = pc; fast[2 * i + 1] = (u32)(uintptr_t)(entry + JIT_PROLOGUE_BYTES); }
  for (auto& l : links) {   // outgoing chains
    if (isSpin(l.targetPc)) continue;
    Entry& t = table[(l.targetPc >> 2) & 0xFFFF];
    if (t.pc == l.targetPc && t.code) { be->patchBranch(entry + 4 * l.wordIdx, t.code + JIT_PROLOGUE_BYTES); linked++; }
    else pending.emplace(l.targetPc, entry + 4 * l.wordIdx);
  }
  auto range = pending.equal_range(pc);   // incoming chains (a block can also branch to itself)
  for (auto it = range.first; it != range.second; ++it) { be->patchBranch(it->second, entry + JIT_PROLOGUE_BYTES); linked++; }
  pending.erase(range.first, range.second);
  return e2;
}

// ---------------- native ARMv7 backend ----------------
#if defined(__arm__)
static u32 h_exec(CPU* c, u32 op, u32 pc) { c->pc = pc; u32 npc = pc + 4; c->exec(op, npc); return npc; }
static u32 h_r8(CPU* c, u32 ea) { return c->bus->r8(ea); }
static u32 h_r16(CPU* c, u32 ea) { return c->bus->r16(ea); }
static u32 h_r32(CPU* c, u32 ea) { return c->bus->r32(ea); }
static void h_w8(CPU* c, u32 ea, u32 v) { c->bus->w8(ea, v); }
static void h_w16(CPU* c, u32 ea, u32 v) { c->bus->w16(ea, v); }
static void h_w32(CPU* c, u32 ea, u32 v) { c->bus->w32(ea, v); }
struct NativeBackend : JitBackend {
  u8* mem = nullptr; size_t cap = 24u << 20, used = 0;
  NativeBackend() { mem = (u8*)mmap(nullptr, cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (mem == MAP_FAILED) mem = nullptr; }
  JitHelpers helpers() override { return {(u32)&h_exec, (u32)&h_r8, (u32)&h_r16, (u32)&h_r32, (u32)&h_w8, (u32)&h_w16, (u32)&h_w32}; }
  u8* install(const u32* w, size_t n) override {
    if (!mem || used + n * 4 > cap) return nullptr;
    u8* p = mem + used; memcpy(p, w, n * 4); used += (n * 4 + 15) & ~15u;
    __builtin___clear_cache((char*)p, (char*)p + n * 4); return p;
  }
  void flush() override { used = 0; }
  u32 mapFast(u32* host, size_t) override { return (u32)(uintptr_t)host; }
  void patchBranch(u8* at, u8* target) override { u32 w = 0xEA000000 | (((int)(target - at) - 8) >> 2 & 0x00FFFFFF); memcpy(at, &w, 4); __builtin___clear_cache((char*)at, (char*)at + 4); }
  void run(CPU* c, u8* ram, u8* entry) override { ((void (*)(CPU*, u8*))entry)(c, ram); }
};
JitBackend* makeNativeBackend() { auto* b = new NativeBackend(); if (!b->mem) { delete b; return nullptr; } return b; }
bool jitNativeAvailable() { return true; }
#else
JitBackend* makeNativeBackend() { return nullptr; }
bool jitNativeAvailable() { return false; }
#endif
