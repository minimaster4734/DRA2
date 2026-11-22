// Full-game JIT smoke test on x86: runs the translated ARM code under Unicorn.
#include <unicorn/unicorn.h>
#include "../core/jit_rt.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cstdlib>
static const u64 CTX = 0x10000000, RAM = 0x20000000, CODE = 0x30000000, STACK = 0x40000000, FAST = 0x50000000, MAGIC = 0x70000000, RET = 0x71000000;
extern JitBackend* (*g_backendFactory)();
static u64 g_cnt[64*1024];
struct Dump { ~Dump(){ if (g_jit) fprintf(stderr, "jit: blocks=%llu linked=%llu flushes=%llu\n", (unsigned long long)g_jit->translated, (unsigned long long)g_jit->linked, (unsigned long long)g_jit->flushes); if(!getenv("HCNT")) return; std::vector<std::pair<u64,int>> v; for(int i=0;i<65536;i++) if(g_cnt[i]) v.push_back({g_cnt[i],i}); std::sort(v.rbegin(),v.rend()); for(int i=0;i<25&&i<(int)v.size();i++) fprintf(stderr,"HELPER op=%d xo=%d n=%llu\n",v[i].second/1024,v[i].second%1024,(unsigned long long)v[i].first);} } g_dump;
struct UcBackend : JitBackend {
  uc_engine* uc; CPU* cpu = nullptr; u8* ram = nullptr; u8* ctxBase = nullptr; size_t used = 0, cap = 32u << 20;
  u64 runs = 0;
  UcBackend() {
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    uc_mem_map(uc, CODE, cap, UC_PROT_ALL); uc_mem_map(uc, STACK, 0x10000, UC_PROT_ALL);
    uc_mem_map(uc, MAGIC, 0x1000, UC_PROT_ALL); uc_mem_map(uc, RET, 0x1000, UC_PROT_ALL);
    for (int i = 0; i < 16; i++) { u32 nop = 0xE320F000; uc_mem_write(uc, MAGIC + i * 16, &nop, 4); }
    uc_hook hh; uc_hook_add(uc, &hh, UC_HOOK_CODE, (void*)hook, this, MAGIC, MAGIC + 0xFF);
  }
  static void hook(uc_engine* u, u64 addr, u32, void* ud) {
    UcBackend* b = (UcBackend*)ud; CPU* c = b->cpu;
    u32 r1, r2, r3, lr; uc_reg_read(u, UC_ARM_REG_R1, &r1); uc_reg_read(u, UC_ARM_REG_R2, &r2); uc_reg_read(u, UC_ARM_REG_LR, &lr);
    u32 ret = 0;
    switch ((addr - MAGIC) / 16) {
      case 0: { { u32 k=(r1>>26)*1024+((r1>>1)&1023); if((r1>>26)==63||(r1>>26)==59||(r1>>26)==4) k=(r1>>26)*1024+((r1>>1)&31); g_cnt[k]++; } u32 npc = r2 + 4; c->pc = r2; c->exec(r1, npc); ret = npc; break; }
      case 1: ret = c->bus->r8(r1); break; case 2: ret = c->bus->r16(r1); break; case 3: ret = c->bus->r32(r1); break;
      case 4: c->bus->w8(r1, r2); break; case 5: c->bus->w16(r1, r2); break; case 6: c->bus->w32(r1, r2); break;
    }
    uc_reg_write(u, UC_ARM_REG_R0, &ret); uc_reg_write(u, UC_ARM_REG_PC, &lr);
  }
  void attach(CPU* c) {
    cpu = c; ram = c->bus->ram;
    uintptr_t a = (uintptr_t)c & ~(uintptr_t)4095, e = ((uintptr_t)c + sizeof(CPU) + 4095) & ~(uintptr_t)4095;
    ctxBase = (u8*)a;
    uc_err r = uc_mem_map_ptr(uc, CTX, e - a, UC_PROT_ALL, (void*)a); if (r) { fprintf(stderr, "ctx map: %s\n", uc_strerror(r)); exit(1); }
    r = uc_mem_map_ptr(uc, RAM, (RAM_SIZE + 4095) & ~4095u, UC_PROT_ALL, ram); if (r) { fprintf(stderr, "ram map: %s\n", uc_strerror(r)); exit(1); }
  }
  JitHelpers helpers() override { return {(u32)MAGIC, (u32)MAGIC + 16, (u32)MAGIC + 32, (u32)MAGIC + 48, (u32)MAGIC + 64, (u32)MAGIC + 80, (u32)MAGIC + 96}; }
  u8* install(const u32* w, size_t n) override {
    if (used + n * 4 > cap) return nullptr;
    uc_mem_write(uc, CODE + used, w, n * 4); u8* t = (u8*)(CODE + used); used += (n * 4 + 15) & ~15u; return t;
  }
  void flush() override { used = 0; uc_ctl_flush_tb(uc); }
  u32 mapFast(u32* host, size_t bytes) override { uc_err r = uc_mem_map_ptr(uc, FAST, bytes, UC_PROT_ALL, host); if (r) { fprintf(stderr, "fast map: %s\n", uc_strerror(r)); exit(1); } return FAST; }
  void patchBranch(u8* at, u8* target) override { u32 w = 0xEA000000 | (((int)((u32)(uintptr_t)target - (u32)(uintptr_t)at) - 8) >> 2 & 0x00FFFFFF); uc_mem_write(uc, (u64)(uintptr_t)at, &w, 4); uc_ctl_remove_cache(uc, (u64)(uintptr_t)at, (u64)(uintptr_t)at + 4); }
  void run(CPU* c, u8*, u8* entry) override {
    if (!cpu) attach(c);
    u32 r0 = CTX + ((u8*)c - ctxBase), r1 = RAM, sp = STACK + 0x8000, lr = RET;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0); uc_reg_write(uc, UC_ARM_REG_R1, &r1); uc_reg_write(uc, UC_ARM_REG_SP, &sp); uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    static bool fl = false;
    uc_err e = uc_emu_start(uc, (u64)(uintptr_t)entry, RET, 0, 0);
    if (e) { fprintf(stderr, "unicorn: %s at entry %p guest pc=%08X\n", uc_strerror(e), entry, c->pc); c->halt("unicorn error"); }
    (void)fl; runs++;
  }
};
static JitBackend* make() { return new UcBackend(); }
struct Init { Init() { g_backendFactory = make; } } g_init;
