// Render-safety tests: every pose/act/stage of the pet and the face/app pages
// with extreme data, drawn into a canvas surrounded by canary memory. Any
// out-of-bounds write (which would corrupt PSRAM on the watch) fails the test.
#include <unity.h>
#include <string.h>
#include <vector>
#include "px.h"
#include "petart.h"
#include "scenes.h"

namespace {
const int kGuard = 4096;             // pixels of canary on each side
const uint16_t kCanary = 0xBEEF;

struct Guarded {
  std::vector<uint16_t> mem;
  px::Canvas c;
  Guarded() : mem(kGuard * 2 + scenes::W * scenes::H, kCanary),
              c(mem.data() + kGuard, scenes::W, scenes::H) {}
  bool intact() const {
    for (int i = 0; i < kGuard; i++) {
      if (mem[i] != kCanary) return false;
      if (mem[mem.size() - 1 - i] != kCanary) return false;
    }
    return true;
  }
};
}  // namespace

void test_every_pose_stays_in_bounds() {
  Guarded g;
  int frames = 0;
  for (int st = 0; st < (int)pet::Stage::Count; st++)
    for (int m = 0; m < (int)pet::Mood::Count; m++)
      for (int a = 0; a < (int)petart::Act::Count; a++)
        for (int flags = 0; flags < 4; flags++)
          for (int acc = 0; acc < (int)pet::Accessory::Count; acc += 2)
            for (uint32_t t = 0; t < 7000; t += 137) {
              petart::Pose p;
              p.stage = (pet::Stage)st;
              p.mood = (pet::Mood)m;
              p.act = (petart::Act)a;
              p.asleep = flags & 1;
              p.sulking = flags & 2;
              p.acc = (pet::Accessory)acc;
              p.clockMs = t * 3 + 11;
              p.animMs = t;
              p.eggPct = (uint8_t)(t % 101);
              // at the screen edges and at every scale the watch uses
              petart::draw(g.c, p, -30 + (int)(t % 300), -20 + (int)(t % 330), 1 + (int)(t % 6));
              frames++;
            }
  printf("  %d frames drawn\n", frames);
  TEST_ASSERT_TRUE(g.intact());
}

void test_icons_and_text_stay_in_bounds() {
  Guarded g;
  for (int i = 0; i < (int)petart::Icon::Count; i++)
    for (int s = 1; s <= 6; s++) petart::drawIcon(g.c, (petart::Icon)i, 230 - s * 4, 270 - s * 3, s);
  px::text(g.c, "WALK TO HATCH! ^~@#$&{}|`", -20, 275, 3, 0xFFFF);
  px::text(g.c, "12:34", 200, -10, 6, 0xFFFF, px::Font::Big);
  px::textC(g.c, "THIS IS A VERY LONG LINE OF TEXT THAT RUNS OFF", 120, 140, 4, 0x1234);
  px::dither(g.c, -50, -50, 400, 400, 0x1111, 0x2222, 9);
  px::roundRect(g.c, 230, 270, 40, 40, 3, 0x3333);
  TEST_ASSERT_TRUE(g.intact());
}

void test_face_and_app_with_extreme_data() {
  Guarded g;
  for (int k = 0; k < 64; k++) {
    scenes::FaceData d;
    d.rtcOk = (k % 5) != 0;
    d.clockUnset = !d.rtcOk;
    d.hour = (uint8_t)((k * 7) % 24); d.minute = (uint8_t)((k * 13) % 60); d.second = (uint8_t)k;
    d.weekday = (uint8_t)(k % 9); d.day = (uint8_t)(k % 32); d.month = (uint8_t)(k % 14);
    d.batOk = k % 2; d.batPct = (uint8_t)(k * 7 % 101);
    d.stepsToday = (k % 3 == 0) ? 4294967295u : (uint32_t)k * 1234567u;
    d.goal = (uint16_t)(k % 4 == 0 ? 0 : 20000);
    d.toSnack = (uint16_t)(k * 97 % 401);
    d.snackPct = (uint8_t)(k * 13 % 101);
    d.pantry = (uint8_t)(k % 5);
    d.walking = k % 2;
    d.pose.stage = (pet::Stage)(k % 5);
    d.pose.mood = (pet::Mood)(k % 6);
    d.pose.act = (petart::Act)(k % 7);
    d.pose.clockMs = (uint32_t)k * 777;
    d.name = (k % 2) ? "BISCUIT" : "FIG";
    d.toast = (k % 3) ? "HI! I'M BISCUIT" : nullptr;
    d.toastAgeMs = (uint32_t)k * 100;
    scenes::drawFaceAll(g.c, d, (uint32_t)k * 1000);
    scenes::AppData a;
    a.face = d;
    a.fullness = (float)(k * 3 % 140) - 20.0f;
    a.lifetimeSteps = 4294967295u;
    a.toNextStage = (k % 2) ? 0 : 123456;
    a.stagePct = (uint8_t)(k * 5 % 120);
    a.resetArmPct = (uint8_t)(k * 3 % 101);
    for (int i = 0; i < 7; i++) a.week[i] = (k % 4 == 1) ? 0 : (uint32_t)(i + 1) * (uint32_t)k * 4567u;
    a.weekDay0 = (uint8_t)(k % 7);
    for (int pg = 0; pg < (int)scenes::Page::Count; pg++) scenes::drawApp(g.c, (scenes::Page)pg, a, (uint32_t)k * 333);
  }
  TEST_ASSERT_TRUE(g.intact());
}

void test_thousands_formatting() {
  char b[16];
  px::fmtThousands(b, sizeof b, 0);          TEST_ASSERT_EQUAL_STRING("0", b);
  px::fmtThousands(b, sizeof b, 999);        TEST_ASSERT_EQUAL_STRING("999", b);
  px::fmtThousands(b, sizeof b, 1000);       TEST_ASSERT_EQUAL_STRING("1,000", b);
  px::fmtThousands(b, sizeof b, 4294967295u); TEST_ASSERT_EQUAL_STRING("4,294,967,295", b);
  char tiny[4];
  px::fmtThousands(tiny, sizeof tiny, 12345); TEST_ASSERT_EQUAL_STRING("", tiny);   // never overflows
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_pose_stays_in_bounds);
  RUN_TEST(test_icons_and_text_stay_in_bounds);
  RUN_TEST(test_face_and_app_with_extreme_data);
  RUN_TEST(test_thousands_formatting);
  return UNITY_END();
}
