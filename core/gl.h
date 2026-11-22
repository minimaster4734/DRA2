// OpenGL ES 2.0 renderer for the GX command stream.
#pragma once
#include "gc.h"
#include <map>
#include <string>
#include <unordered_map>

struct GXVertex {
  float pos[3]; float nrm[3]; float col[2][4]; float tex[8][2];
  u8 pmi, tmi[8]; bool hasPmi, hasTmi[8], hasCol[2], hasNrm, hasTex[8];
};

struct Renderer {
  Machine* m = nullptr;
  bool init(Machine* mach, bool showWindow);
  void shutdown();
  void draw(int prim, const GXVertex* v, int n);   // prim = (cmd>>3)&7 : 0 quads 2? see gx.cpp
  void efbCopy(u32 val);
  void tlutLoad(u32 ramAddr, u32 tmemOff, u32 count);
  u64 frame = 0;
  void present();
  bool saveShot(const char* path);
  void pollEvents();
  // internals
  struct Impl; Impl* d = nullptr;
};
