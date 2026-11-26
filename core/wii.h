// WiiWare runtime layer: WAD container, IOS (IPC) high-level emulation, Hollywood registers.
// Clean-room: no Nintendo code or keys are included. The user supplies their own WAD and their own common key.
#pragma once
#include "gc.h"
#include <map>
#include <deque>

struct WiiContent { u32 cid; u16 index, type; u64 size; u8 sha[20]; u64 fileOff; };

struct Wii {
  Machine* m = nullptr;
  std::string wadPath, nandDir, err;
  FILE* wad = nullptr;
  u8 titleKey[16] = {};
  u64 titleId = 0; u32 iosNum = 0;
  std::vector<WiiContent> contents;
  std::vector<u8> tmd;

  bool open(const char* wadFile, const u8 key16[16]);                 // parse + decrypt title key; false -> err
  bool readContent(int index, std::vector<u8>& out);                  // decrypted + sha1 verified
  bool findGameDol(std::vector<u8>& dol);                             // the LZ11 packed main executable
  bool boot(Machine* mach);                                           // load DOL, lowmem, IOS state
  // IPC
  u32 ppcMsg = 0, ppcCtrl = 0, armMsg = 0, armCtrl = 0;
  std::deque<u32> replies; u64 replyDue = 0;
  bool irq() const;
  void ipcWrite(u32 off, u32 val);
  u32 ipcRead(u32 off);
  void tick();
  void handle(u32 addr);
  void reply(u32 addr, s32 result, u64 delay = 4000);
  void updateDue();
  // device layer
  struct Fd { int kind = 0; std::string path; u32 pos = 0; FILE* f = nullptr; int mode = 0; std::vector<u8> data; int cidx = -1; };
  std::map<int, Fd> fds; int nextFd = 1;
  s32 devIoctl(Fd& fd, u32 addr, bool& defer);
  s32 devIoctlv(Fd& fd, u32 addr, bool& defer);
  std::string hostPath(const std::string& p);
  u32 hw[0x2000 / 4 * 4] = {};
  std::vector<u32> pendingBt;
  // Bluetooth / Wii Remote (usb oh1 57e/305)
  struct Bt* bt = nullptr;
  u32 btRead(u32) { return 0; }
  u16 remoteButtons = 0;   // Wii Remote core buttons (host driven)
  u8 remoteAccel[3] = {128, 128, 160};
  bool trace = false;
};
void wiiSetRemote(Machine* m, u16 buttons);
