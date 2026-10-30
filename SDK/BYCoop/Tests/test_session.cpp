// Host-side test: 1 host + 3 joiners over real UDP loopback, plus a 5th rejected (full),
// a version-mismatch-free leave/rejoin, pose relay convergence and appearance propagation.
#include "../Source/Net/Session.h"
#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>
#include <set>
using namespace coop;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok: %s\n", m); } while (0)
static double T() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void Run(std::vector<Session*> s, double secs, int frame = 0) {
  double end = T() + secs;
  while (T() < end) {
    double now = T();
    for (size_t i = 0; i < s.size(); ++i) {
      Pose p; p.pos[0] = (float)i * 10 + (float)std::sin(now); p.pos[2] = (float)i; p.yaw = (float)i;
      s[i]->SetLocalPose(p); s[i]->Update(now);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}
int main() {
  std::vector<Session*> s;
  std::string err;
  for (int i = 0; i < 5; ++i) {
    s.push_back(new Session(CreateUdpTransport(27100 + i)));
    if (!s[i]->Start(err)) { printf("init %s\n", err.c_str()); return 1; }
    Appearance a; a.gender = i & 1; a.hide = (uint8_t)(i * 3); snprintf(a.name, sizeof a.name, "cow%d", i);
    s[i]->SetLocalAppearance(a);
  }
  s[0]->Host();
  for (int i = 1; i < 4; ++i) s[i]->Join("127.0.0.1:27100");
  Run({s[0], s[1], s[2], s[3]}, 1.5);
  std::set<int> slots;
  for (int i = 0; i < 4; ++i) slots.insert(s[i]->LocalSlot());
  CHECK(slots.size() == 4 && *slots.begin() == 0 && *slots.rbegin() == 3, "4 players got unique slots 0..3");
  for (int i = 0; i < 4; ++i) CHECK(s[i]->Remotes().size() == 3, ("player sees 3 remotes: " + std::to_string(i)).c_str());
  bool looks = true, poses = true;
  for (int i = 0; i < 4; ++i)
    for (auto& [slot, r] : s[i]->Remotes()) {
      int owner = -1; for (int j = 0; j < 4; ++j) if (s[j]->LocalSlot() == slot) owner = j;
      char nm[24]; snprintf(nm, sizeof nm, "cow%d", owner);
      looks &= strcmp(r.look.name, nm) == 0 && r.look.hide == owner * 3 && r.look.gender == (owner & 1);
      Pose p = r.Sample(T(), 0.1);
      poses &= r.hasPose && std::fabs(p.pos[2] - owner) < 1e-4 && std::fabs(p.pos[0] - owner * 10) < 2.0;
    }
  CHECK(looks, "appearance (gender/hide/name) propagated to everyone");
  CHECK(poses, "poses relayed via host and converge per slot");
  // world transfer: joiner asks, host answers with a save-slot sized blob (0xd170 bytes)
  s[1]->RequestWorld();
  Run({s[0], s[1]}, 0.3);
  PeerId req = 0;
  CHECK(s[0]->PopWorldRequest(req) && req != 0, "host gets the world request");
  WorldData wd; wd.ready = true; wd.level = 7; wd.pos[1] = 2.5f; wd.slot.resize(0xd170);
  for (size_t i = 0; i < wd.slot.size(); ++i) wd.slot[i] = (uint8_t)(i * 31);
  s[0]->SendWorld(req, wd);
  Run({s[0], s[1]}, 0.5);
  WorldData got;
  bool okw = s[1]->PopWorld(got);
  CHECK(okw && got.ready && got.level == 7 && got.pos[1] == 2.5f && got.slot == wd.slot, "joiner receives the whole world blob");
  s[0]->BroadcastWorldReady();
  Run({s[0], s[1], s[2], s[3]}, 0.3);
  CHECK(s[2]->PopWorldReady() && !s[2]->PopWorldReady(), "world-ready broadcast reaches joiners once");
  s[4]->Join("127.0.0.1:27100");
  Run({s[0], s[1], s[2], s[3], s[4]}, 1.5);
  CHECK(s[4]->LocalSlot() < 0, "5th player rejected (full)");
  // change look mid-session
  Appearance a2; a2.gender = 1; a2.hide = 42; strcpy(a2.name, "renamed");
  s[2]->SetLocalAppearance(a2);
  Run({s[0], s[1], s[2], s[3]}, 1.0);
  int slot2 = s[2]->LocalSlot();
  CHECK(s[1]->Remotes().at(slot2).look.hide == 42, "appearance change propagates");
  // leave + rejoin frees slot
  s[3]->Leave();
  Run({s[0], s[1], s[2], s[3]}, 1.0);
  CHECK(s[1]->Remotes().size() == 2, "leave removes player from others");
  s[4]->Leave(); s[4]->Join("127.0.0.1:27100");
  Run({s[0], s[1], s[2], s[4]}, 1.5);
  CHECK(s[4]->LocalSlot() > 0, "freed slot reused by new joiner");
  // host timeout: stop pumping host
  Run({s[1], s[2], s[4]}, 11.0);
  CHECK(s[1]->LocalSlot() < 0 && s[1]->Remotes().empty(), "joiners notice dead host");
  for (auto* x : s) delete x;
  printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
  return fails != 0;
}
