// JIT runtime: block cache and dispatcher. The code-memory/execution backend is abstract so the same runtime can run
// natively on ARMv7 and under the Unicorn-based test harness on x86.
#pragma once
#include "jit.h"
#include <vector>
#include <unordered_map>

struct JitBackend {
  virtual ~JitBackend() {}
  virtual JitHelpers helpers() = 0;
  virtual u8* install(const u32* words, size_t n) = 0;   // copy code into executable memory; returns an entry token
  virtual void flush() = 0;
  virtual u32 mapFast(u32* host, size_t bytes) = 0;        // make the table visible to generated code; returns its address there
  virtual void patchBranch(u8* at, u8* target) = 0;      // rewrite the A32 `b` at `at` to jump to `target`                               // drop all installed code
  virtual void run(CPU* c, u8* ram, u8* entry) = 0;       // execute one block
};

struct JitRT {
  JitBackend* be = nullptr;
  struct Entry { u32 pc = 1; u8* code = nullptr; };
  std::vector<Entry> table = std::vector<Entry>(1 << 16);
  std::vector<u8> codePage = std::vector<u8>(RAM_SIZE / 4096);
  u64 translated = 0, flushes = 0, linked = 0;
  CPU* cpu = nullptr; u32* fast = nullptr;   // fast[2*i] = pc, fast[2*i+1] = chain entry of the block cached in slot i
  std::unordered_multimap<u32, u8*> pending;   // target pc -> unlinked branch sites waiting for it
  bool isSpin(u32 pc) const;
  void invalidateAll();
  void invalidateRange(u32 pa, u32 len);
  Entry& lookup(CPU* c, u32 pc);   // translate on demand
};
extern JitRT* g_jit;
bool jitNativeAvailable();
JitBackend* makeNativeBackend();   // ARMv7 only; nullptr elsewhere
