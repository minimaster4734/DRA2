// Bluetooth HCI + L2CAP + HID emulation of one Wii Remote behind IOS' /dev/usb/oh1/57e/305.
// Written from the public Bluetooth/HID specifications and the Wiibrew protocol notes; no Nintendo code.
#include "wii.h"
#include <deque>

struct Bt {
  Wii* w;
  struct Rd { u32 addr, dst, len; };
  std::deque<Rd> evtRd, aclRd;
  std::deque<std::vector<u8>> evtQ, aclQ;
  bool scanOn = false, connReqSent = false, connected = false, authPending = false;
  u16 handle = 0x0100;
  u16 hostCtrl = 0, hostIntr = 0; bool ctrlUp = false, intrUp = false, ctrlCfgOut = false, intrCfgOut = false, ctrlCfgIn = false, intrCfgIn = false;
  u8 idCtr = 1; u64 connDue = 0;
  u8 mode = 0x30; bool continuous = false; u8 leds = 0; u64 nextRep = 0; bool statusFlagExt = false;
  u8 eeprom[0x1700] = {};
  const u8 wiimote[6] = {0xCC, 0xBB, 0xAA, 0x1D, 0x19, 0x00};   // 00:19:1D:AA:BB:CC (little endian on the wire)
  const u8 hostAddr[6] = {0x33, 0x22, 0x11, 0xAB, 0x17, 0x00};

  Bt(Wii* ww) : w(ww) {
    u8 cal[10] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0, 0}; u32 s = 0x55; for (int i = 0; i < 8; i++) s += cal[i]; cal[8] = cal[9] = (u8)s;
    memcpy(eeprom + 0x16, cal, 10); memcpy(eeprom + 0x20, cal, 10);
  }
  void evt(u8 code, std::initializer_list<u8> p) { std::vector<u8> v; v.push_back(code); v.push_back((u8)p.size()); v.insert(v.end(), p.begin(), p.end()); evtQ.push_back(v); }
  void evtv(u8 code, const std::vector<u8>& p) { std::vector<u8> v; v.push_back(code); v.push_back((u8)p.size()); v.insert(v.end(), p.begin(), p.end()); evtQ.push_back(v); }
  void cc(u16 op, std::vector<u8> ret) { std::vector<u8> p = {1, (u8)op, (u8)(op >> 8), 0}; p.insert(p.end(), ret.begin(), ret.end()); evtv(0x0E, p); }
  void cs(u16 op, u8 st = 0) { evt(0x0F, {st, 1, (u8)op, (u8)(op >> 8)}); }

  void acl(u16 cid, const std::vector<u8>& payload) {   // device -> host
    std::vector<u8> v; u16 l = payload.size();
    v.push_back(handle & 0xFF); v.push_back((handle >> 8) | 0x20);   // PB = first automatically flushed
    u16 tot = l + 4; v.push_back(tot & 0xFF); v.push_back(tot >> 8);
    v.push_back(l & 0xFF); v.push_back(l >> 8); v.push_back(cid & 0xFF); v.push_back(cid >> 8);
    v.insert(v.end(), payload.begin(), payload.end()); aclQ.push_back(v);
  }
  void sig(u8 code, u8 id, std::vector<u8> d) { std::vector<u8> p = {code, id, (u8)d.size(), (u8)(d.size() >> 8)}; p.insert(p.end(), d.begin(), d.end()); acl(1, p); }
  void connectReq(u16 psm, u16 scid) { sig(0x02, idCtr++, {(u8)psm, (u8)(psm >> 8), (u8)scid, (u8)(scid >> 8)}); }
  void cfgReq(u16 dcid) { sig(0x04, idCtr++, {(u8)dcid, (u8)(dcid >> 8), 0, 0, 1, 2, 0xB9, 0}); }

  // ---- HID (device side)
  bool paired = false; bool l2Started = false; bool statusSent = false; u64 reconnDue = 0; u64 l2Due = 0;
  // GameCube-style input (touch overlay / keyboard / gamepad) -> Wii Remote buttons, remote held sideways (screen-left = remote Up ...)
  u16 buttons() const {
    Machine* m = w->m; u16 p = m->padBtn; if (w->remoteButtons) p |= w->remoteButtons;
    bool L = (p & 0x0100) || m->padStick[0] < 64, R = (p & 0x0200) || m->padStick[0] > 192, D = (p & 0x0400) || m->padStick[1] < 64, U = (p & 0x0800) || m->padStick[1] > 192;
    static const bool side = !GENV("WIIUPRIGHT");
    u8 hi = 0, lo = 0;
    if (side) { if (L) hi |= 0x08; if (R) hi |= 0x04; if (U) hi |= 0x02; if (D) hi |= 0x01; }   // Up/Down/Left/Right bits: 08/04/01/02
    else { if (L) hi |= 0x01; if (R) hi |= 0x02; if (U) hi |= 0x08; if (D) hi |= 0x04; }
    if (p & 0x0010) hi |= 0x10;                       // +
    if (p & 0x0001) lo |= 0x01;                       // 2
    if (p & 0x0002) lo |= 0x02;                       // 1
    if (p & 0x0008) lo |= 0x04;                       // B
    if (p & 0x0004) lo |= 0x08;                       // A
    if (p & 0x1000) lo |= 0x10;                       // -
    if (p & 0x4000) lo |= 0x80;                       // Home
    return (u16)(hi << 8 | lo);
  }
  void sendInput(const std::vector<u8>& rep) { if (w->trace && !rep.empty() && rep[0] != 0x30 && rep[0] != 0x31) w->m->log("BT HID in report %02X len=%zu intrUp=%d", rep[0], rep.size(), (int)intrUp); if (!intrUp) return; std::vector<u8> p = {0xA1}; p.insert(p.end(), rep.begin(), rep.end()); acl(hostIntr, p); }
  void sendMode() {
    std::vector<u8> r = {mode, (u8)(buttons() >> 8), (u8)buttons()};
    r[1] = (u8)(buttons() >> 8); r[2] = (u8)buttons();   // BB BB: first byte holds the D-pad/+ bits, second the rest
    r = {mode, (u8)(buttons() >> 8), (u8)(buttons() & 0xFF)};
    { static int ln = 0; if (buttons() && ln < 12 && w->trace) { ln++; w->m->log("BT input report mode=%02X buttons=%04X sent (intrUp=%d)", mode, buttons(), (int)intrUp); } }
    if (mode == 0x31 || mode == 0x33 || mode == 0x35 || mode == 0x37) { r.push_back(w->remoteAccel[0]); r.push_back(w->remoteAccel[1]); r.push_back(w->remoteAccel[2]); }
    if (mode == 0x32) for (int i = 0; i < 8; i++) r.push_back(0);
    if (mode == 0x33) for (int i = 0; i < 12; i++) r.push_back(0xFF);
    if (mode == 0x35) for (int i = 0; i < 16; i++) r.push_back(0);
    if (mode == 0x36) for (int i = 0; i < 10 + 9; i++) r.push_back(0xFF);
    if (mode == 0x37) for (int i = 0; i < 10 + 6; i++) r.push_back(i < 10 ? 0xFF : 0);
    sendInput(r);
  }
  void sendStatus() { sendInput({0x20, (u8)(buttons() >> 8), (u8)buttons(), (u8)((leds << 4) | (statusFlagExt ? 2 : 0) | 0x0C), 0, 0, 0xC8}); }
  void ack(u8 id, u8 err = 0) { sendInput({0x22, (u8)(buttons() >> 8), (u8)buttons(), id, err}); }
  void hidOut(const u8* d, u32 n) {
    if (n < 2) return; u8 id = d[1];
    switch (id) {
      case 0x10: break;
      case 0x11: leds = d[2] >> 4; ack(id); if (GENV("EXP_A")) sendStatus(); break;
      case 0x12: continuous = d[2] & 4; mode = d[3]; ack(id); break;
      case 0x13: case 0x1A: case 0x14: case 0x19: ack(id); if (GENV("EXP_B") && id == 0x1A) sendStatus(); break;
      case 0x15: sendStatus(); break;
      case 0x16: ack(id); break;
      case 0x17: {   // read memory / registers
        u8 space = d[2]; u32 addr = d[3] << 16 | d[4] << 8 | d[5]; u32 size = d[6] << 8 | d[7];
        u32 done = 0; if (!size) break;
        if (addr == 0x1770 && GENV("R17")) { int m17 = atoi(GENV("R17")); if (m17 == 3) break; std::vector<u8> r = {0x21, (u8)(buttons() >> 8), (u8)buttons(), (u8)(((std::min<u32>(size, 16) - 1) << 4) | (m17 == 1 ? 8 : m17 == 2 ? 7 : 0)), (u8)(addr >> 8), (u8)addr}; for (int i = 0; i < 16; i++) r.push_back(m17 == 4 ? 0xFF : 0); sendInput(r); break; }
        if (!(space & 4) && addr + size > sizeof eeprom) { std::vector<u8> r = {0x21, (u8)(buttons() >> 8), (u8)buttons(), (u8)(((std::min<u32>(size, 16) - 1) << 4) | 8), (u8)(addr >> 8), (u8)addr}; for (int i = 0; i < 16; i++) r.push_back(0); sendInput(r); break; }   // beyond the EEPROM: real remotes answer error 8 (the SDK probes 0x1770)
        while (done < size) { u32 chunk = std::min<u32>(16, size - done); std::vector<u8> r = {0x21, (u8)(buttons() >> 8), (u8)buttons(), (u8)(((chunk - 1) << 4) | ((space & 4) ? 7 : 0)), (u8)((addr + done) >> 8), (u8)(addr + done)};
          for (u32 i = 0; i < 16; i++) r.push_back((space & 4) || i >= chunk ? 0 : (addr + done + i < sizeof eeprom ? eeprom[addr + done + i] : 0)); sendInput(r); done += chunk; }
        break; }
      default: break;
    }
  }

  // ---- L2CAP (host -> device)
  void l2cap(u16 cid, const u8* d, u32 n) {
    if (cid == 1) {   // signalling
      u32 p = 0; while (p + 4 <= n) { u8 code = d[p], id = d[p + 1]; u16 len = d[p + 2] | d[p + 3] << 8; const u8* q = d + p + 4; if (p + 4 + len > n) break;
        if (code == 0x03 && len >= 8) { u16 dcid = q[0] | q[1] << 8, scid = q[2] | q[3] << 8, res = q[4] | q[5] << 8;   // connect response
          if (res == 0) { if (scid == 0x40) { hostCtrl = dcid; cfgReq(hostCtrl); } else if (scid == 0x41) { hostIntr = dcid; cfgReq(hostIntr); } } }
        else if (code == 0x04 && len >= 4) { u16 dcid = q[0] | q[1] << 8;   // host's config request -> accept
          sig(0x05, id, {(u8)(dcid == 0x40 ? hostCtrl : hostIntr), (u8)((dcid == 0x40 ? hostCtrl : hostIntr) >> 8), 0, 0, 0, 0});
          if (dcid == 0x40) ctrlCfgIn = true; else intrCfgIn = true; chanCheck(); }
        else if (code == 0x05 && len >= 6) { u16 scid = q[0] | q[1] << 8; if (scid == 0x40) ctrlCfgOut = true; else if (scid == 0x41) intrCfgOut = true;   /* scid in a config response is OUR channel id */ chanCheck(); }
        else if (code == 0x02 && len >= 4) { u16 psm = q[0] | q[1] << 8, scid = q[2] | q[3] << 8; (void)psm; sig(0x03, id, {0, 0, (u8)scid, (u8)(scid >> 8), 2, 0, 0, 0}); }   // host-initiated: refuse
        else if (code == 0x08) sig(0x09, id, std::vector<u8>(q, q + len));
        else if (code == 0x0A) sig(0x0B, id, {q[0], q[1], 1, 0});
        else if (code == 0x06) sig(0x07, id, {q[0], q[1], q[2], q[3]});
        p += 4 + len; }
      return; }
    if ((cid == 0x40 || cid == 0x41) && n >= 1) { if ((d[0] & 0xF0) == 0xA0) hidOut(d, n); }   // DATA output
  }
  void chanCheck() {
    if (ctrlCfgIn && ctrlCfgOut && !ctrlUp) { ctrlUp = true; connectReq(0x13, 0x41); }
    if (intrCfgIn && intrCfgOut && !intrUp) { intrUp = true; connected = true; w->m->log("BT: Wii Remote channels up"); nextRep = w->m->cpu.icount + 200000; }
  }
  void aclOut(const u8* d, u32 n) {
    if (w->trace) { std::string h; for (u32 i = 0; i < n && i < 40; i++) { char t[4]; snprintf(t, 4, "%02X", d[i]); h += t; } w->m->log("BT ACL out %u bytes: %s", n, h.c_str()); }
    evt(0x13, {1, (u8)handle, (u8)(handle >> 8), 1, 0});   // Number_Of_Completed_Packets: give the host its ACL buffer credit back (it only has 10)
    if (n < 8) return; u16 len = d[4] | d[5] << 8, cid = d[6] | d[7] << 8; if (len + 8 > n) len = n - 8; l2cap(cid, d + 8, len);
  }

  // ---- HCI (host -> controller)
  void hci(const u8* d, u32 n) {
    if (n < 3) return; u16 op = d[0] | d[1] << 8; const u8* p = d + 3;
    if (w->trace) w->m->log("BT HCI cmd %04X len=%u", op, n - 3);
    switch (op) {
      case 0x0C03: scanOn = false; cc(op, {}); break;
      case 0x1001: cc(op, {3, 0x5C, 0x0C, 3, 0x0F, 0, 0x0E, 0x43}); break;
      case 0x1002: cc(op, std::vector<u8>(64, 0)); break;
      case 0x1003: cc(op, {0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80}); break;
      case 0x1005: cc(op, {0x53, 0x01, 64, 10, 0, 0, 0}); break;
      case 0x1009: { std::vector<u8> r(hostAddr, hostAddr + 6); cc(op, r); break; }
      case 0x0C25: case 0x0C23: cc(op, {0, 0, 0}); break;
      case 0x0C14: cc(op, std::vector<u8>(248, 0)); break;
      case 0x0C1A: scanOn = n > 3 && (p[0] & 2); w->m->log("BT: scan enable %02X", n > 3 ? p[0] : 0xFF); cc(op, {}); if (scanOn && !connReqSent) { connReqSent = true; connDue = w->m->cpu.icount + 3000000; } break;
      case 0x0409: cs(op); if (n > 9 && p[6] == 0) { std::vector<u8> r = {0}; r.insert(r.end(), wiimote, wiimote + 6); r.push_back(0); evtv(0x12, r); }   // role change: we are master
        { std::vector<u8> v = {0, (u8)handle, (u8)(handle >> 8)}; v.insert(v.end(), wiimote, wiimote + 6); v.push_back(1); v.push_back(0); evtv(0x03, v); }  if (!paired) { std::vector<u8> lk(wiimote, wiimote + 6); evtv(0x17, lk); } else if (!l2Started) { l2Started = true; l2Due = w->m->cpu.icount + 6000000; } break;   // Accept_Connection_Request -> Connection_Complete
      case 0x040A: cs(op); break;
      case 0x0411: { cs(op); std::vector<u8> v(wiimote, wiimote + 6); evtv(0x17, v); break; }       // Authentication_Requested -> Link_Key_Request
      case 0x040B: { cc(op, std::vector<u8>(wiimote, wiimote + 6)); std::vector<u8> v = {0, (u8)handle, (u8)(handle >> 8)}; evtv(0x06, v); paired = true; if (!l2Started) { l2Started = true; l2Due = w->m->cpu.icount + 3000000; } break; }   // Link_Key_Request_Reply -> Authentication_Complete
      case 0x040C: { cc(op, std::vector<u8>(wiimote, wiimote + 6)); std::vector<u8> v(wiimote, wiimote + 6); evtv(0x16, v); break; }   // Link_Key_Request_Negative_Reply -> PIN_Code_Request (pairing)
      case 0x040D: { cc(op, std::vector<u8>(wiimote, wiimote + 6));   // PIN_Code_Request_Reply -> Link_Key_Notification + Authentication_Complete
        std::vector<u8> k(wiimote, wiimote + 6); for (int i = 0; i < 16; i++) k.push_back((u8)(0x31 + i * 7)); k.push_back(0); evtv(0x18, k);
        std::vector<u8> v = {0, (u8)handle, (u8)(handle >> 8)}; evtv(0x06, v); paired = true; if (!l2Started) { l2Started = true; l2Due = w->m->cpu.icount + 400000; } break; }
      case 0x040E: cc(op, std::vector<u8>(wiimote, wiimote + 6)); break;
      case 0x0413: cs(op); evt(0x08, {0, (u8)handle, (u8)(handle >> 8), 1}); break;                   // Set_Connection_Encryption -> Encryption_Change
      case 0x0419: { cs(op); std::vector<u8> v = {0}; v.insert(v.end(), wiimote, wiimote + 6); const char* nm = "Nintendo RVL-CNT-01"; for (int i = 0; i < 248; i++) v.push_back(i < (int)strlen(nm) ? nm[i] : 0); evtv(0x07, v); break; }
      case 0x041B: cs(op); evt(0x0B, {0, (u8)handle, (u8)(handle >> 8), 0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00}); break;
      case 0x041D: cs(op); evt(0x0C, {0, (u8)handle, (u8)(handle >> 8), 2, 0x0F, 0, 0x29, 0x02}); break;      // Read_Remote_Version_Info
      case 0x041F: cs(op); evt(0x1C, {0, (u8)handle, (u8)(handle >> 8), 0, 0}); break;                  // Read_Clock_Offset
      case 0x040F: cs(op); evt(0x1D, {0, (u8)handle, (u8)(handle >> 8), 0x18, 0xCC}); break;             // Change_Connection_Packet_Type
      case 0x0C0D: { std::vector<u8> rk = {1}; rk.insert(rk.end(), wiimote, wiimote + 6); for (int i = 0; i < 16; i++) rk.push_back((u8)(0x31 + i * 7)); evtv(0x15, rk);   // Return_Link_Keys: the paired remote
        cc(op, {0xFF, 0, 1, 0}); break; }
      case 0x0C37: cc(op, {(u8)handle, (u8)(handle >> 8)}); break;
      case 0x0401: cs(op); { std::vector<u8> v = {1}; v.insert(v.end(), wiimote, wiimote + 6); v.push_back(1); v.push_back(0); v.push_back(0); v.push_back(0x04); v.push_back(0x25); v.push_back(0); v.push_back(0); v.push_back(0); evtv(0x02, v); } evt(0x01, {0}); break;
      case 0x0406: w->m->log("BT disconnect reason %02X", n > 5 ? p[2] : 0); if (w->trace) { u32 sp = w->m->cpu.gpr[1]; std::string bt; for (int i = 0; i < 14 && sp; i++) { u32 nxt = w->m->bus.r32(sp), lrv = w->m->bus.r32(nxt + 4); char t[16]; snprintf(t, 16, " %08X", lrv); bt += t; sp = nxt; if (nxt < 0x80000000u) break; } w->m->log("BT disc backtrace pc=%08X lr=%08X:%s", w->m->cpu.pc, w->m->cpu.lr, bt.c_str()); } cs(op); evt(0x05, {0, (u8)handle, (u8)(handle >> 8), 0x16}); connected = ctrlUp = intrUp = false; l2Started = false; statusSent = false; connReqSent = false; reconnDue = w->m->cpu.icount + 40000000; ctrlCfgIn = ctrlCfgOut = intrCfgIn = intrCfgOut = false; break;
      case 0x0405: cs(op); break;
      case 0x0C04: cc(op, {}); break;
      case 0x0801: case 0x0802: case 0x080D: cc(op, {(u8)handle, (u8)(handle >> 8)}); break;
      case 0x0809: cc(op, {(u8)handle, (u8)(handle >> 8)}); break;
      case 0x0C2D: case 0x0C28: case 0x0C29: cc(op, {}); break;
      case 0x1007: cc(op, {0}); break;
      case 0x0818: cc(op, {}); break;
      default: if ((op >> 10) == 0x3F) { cc(op, {}); } else cc(op, {}); break;   // everything else: success with no parameters
    }
  }

  void flush() {
    while (!evtQ.empty() && !evtRd.empty()) { Rd r = evtRd.front(); evtRd.pop_front(); auto& v = evtQ.front(); u32 n = std::min<u32>(r.len, (u32)v.size());
      for (u32 i = 0; i < n; i++) w->m->bus.w8(r.dst + i, v[i]); evtQ.pop_front(); w->reply(r.addr, (s32)n); }
    while (!aclQ.empty() && !aclRd.empty()) { Rd r = aclRd.front(); aclRd.pop_front(); auto& v = aclQ.front(); u32 n = std::min<u32>(r.len, (u32)v.size());
      for (u32 i = 0; i < n; i++) w->m->bus.w8(r.dst + i, v[i]); aclQ.pop_front(); w->reply(r.addr, (s32)n); }
  }
  void tick() {
    u64 now = w->m->cpu.icount;
    if (connReqSent && !connected && connDue && now >= connDue) { connDue = 0;
      std::vector<u8> v(wiimote, wiimote + 6); v.push_back(0x04); v.push_back(0x25); v.push_back(0x00); v.push_back(1); evtv(0x04, v); }   // Connection_Request
    if (reconnDue && now >= reconnDue && !connected) { if (scanOn && !connReqSent) { reconnDue = 0; connReqSent = true; connDue = now + 1000000; w->m->log("BT: remote reconnecting"); } else if (!scanOn) reconnDue = now + 10000000; }
    if (l2Started && l2Due && now >= l2Due) { l2Due = 0; connectReq(0x11, 0x40); }
    if (connected && now >= nextRep) { nextRep = now + 3000000; if (!statusSent && !GENV("NOSTATUS")) { statusSent = true; sendStatus(); } if (continuous || true) sendMode(); }
    flush();
  }
};

s32 btIoctlv(Wii& w, u32 a, bool& defer) {
  if (!w.bt) w.bt = new Bt(&w);
  Bt& b = *w.bt; Bus& bus = w.m->bus; u32 cmd = bus.r32(a + 0xC), nin = bus.r32(a + 0x10), nio = bus.r32(a + 0x14), vp = bus.r32(a + 0x18);
  auto V = [&](u32 i, u32& p, u32& l) { p = bus.r32(vp + i * 8); l = bus.r32(vp + i * 8 + 4); };
  u32 n = nin + nio; (void)n;
  if (cmd == 0) {   // control message: HCI command
    u32 p[8] = {}, l[8] = {}; for (u32 i = 0; i < n && i < 8; i++) V(i, p[i], l[i]);
    u8 rt = bus.r8(p[0]), rq = bus.r8(p[1]); u16 len = bus.r8(p[4]) | (bus.r8(p[4] + 1) << 8);   // wLength
    u32 dp = p[6]; (void)rq;
    if (w.trace) w.m->log("BT ctrl rt=%02X rq=%02X len=%u l5=%u l6=%u nin=%u nio=%u", rt, rq, len, l[5], l[6], nin, nio);
    if (len > 0 && len < 512 && !(rt & 0x80)) { std::vector<u8> d(len); for (u32 i = 0; i < len; i++) d[i] = bus.r8(dp + i); b.hci(d.data(), len); }
    b.flush(); return len;
  }
  u32 p0, l0, p1, l1, p2 = 0, l2 = 0; V(0, p0, l0); V(1, p1, l1); if (n > 2) V(2, p2, l2);
  u8 ep = bus.r8(p0); u16 len = bus.r8(p1) | (bus.r8(p1 + 1) << 8); (void)len;
  if (cmd == 1 && ep == 0x02) { std::vector<u8> d(l2); for (u32 i = 0; i < l2; i++) d[i] = bus.r8(p2 + i); b.aclOut(d.data(), l2); b.flush(); return (s32)l2; }
  if (cmd == 1 && ep == 0x82) { b.aclRd.push_back({a, p2, l2}); defer = true; b.flush(); return 0; }
  if (cmd == 2 && ep == 0x81) { b.evtRd.push_back({a, p2, l2}); defer = true; b.flush(); return 0; }
  w.m->log("BT: unhandled usb message cmd=%u ep=%02X", cmd, ep);
  return 0;
}
void btTick(Wii& w) { if (w.bt) w.bt->tick(); }
std::string btDebug(Wii& w) { if (!w.bt) return "nobt"; char t[160]; snprintf(t, sizeof t, "evtQ=%zu evtRd=%zu aclQ=%zu aclRd=%zu connected=%d scan=%d", w.bt->evtQ.size(), w.bt->evtRd.size(), w.bt->aclQ.size(), w.bt->aclRd.size(), (int)w.bt->connected, (int)w.bt->scanOn); return t; }
u64 btNextDue(Wii& w) {
  if (!w.bt) return 0; Bt& b = *w.bt; u64 d = 0; auto mn = [&](u64 v) { if (v && (!d || v < d)) d = v; };
  if (b.connReqSent && !b.connected) mn(b.connDue);
  if (b.reconnDue) mn(b.reconnDue);
  if (b.l2Started) mn(b.l2Due);
  if (b.connected) mn(b.nextRep);
  return d;
}
