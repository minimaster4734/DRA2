// ARMv7 (A32) dynamic recompiler for the Gekko core.
// Generated code works on the CPU context in memory (r4 = CPU*, r5 = RAM base). Anything not translated natively is
// executed through the interpreter by a helper call, so behaviour stays identical to the interpreter.
#pragma once
#include "gc.h"
#include <vector>

struct JitHelpers {   // addresses of the host functions the generated code calls
  u32 exec;           // u32 exec(CPU*, u32 op, u32 pc) -> next pc
  u32 r8, r16, r32;   // u32 rN(CPU*, u32 ea)
  u32 w8, w16, w32;   // void wN(CPU*, u32 ea, u32 val)
};

// Translate the guest block at `pc` (words[i] = decoded guest instruction at pc+4*i). Appends A32 words to `out` and
// returns the number of guest instructions covered. The block is entered as void fn(CPU*, u8* ram) and leaves
// the next guest pc in cpu->pc and the retired instruction count in cpu->jitCount.
struct JitLink { u32 wordIdx; u32 targetPc; };   // a patchable `b` to the block at targetPc (initially pointing at the unlinked exit)
const u32 JIT_PROLOGUE_BYTES = 12;                // chained jumps enter a block after its prologue
int jitTranslate(std::vector<u32>& out, const JitHelpers& hp, u32 pc, const u32* words, int nwords, std::vector<JitLink>* links = nullptr);
