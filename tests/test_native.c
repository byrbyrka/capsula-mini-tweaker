#include "capsule.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned assertions;
#define CHECK(x)                                                               \
  do {                                                                         \
    assert(x);                                                                 \
    assertions++;                                                              \
  } while (0)
static void put(const char *name, const char *text) {
  char p[MAXPATH];
  path_for(p, sizeof p, name);
  CHECK(atomic_text(p, text) == 0);
}
static void tap(App *a, int key, double t) {
  key_event(a, key, 1, t);
  key_event(a, key, 0, t + .1);
}
static void init_app(App *a) {
  memset(a, 0, sizeof *a);
  config_defaults(&a->cfg);
  a->state = (State){50, 0, MODE_FACE};
  put("/hardware", "50 0\n");
  CHECK(audio_open(&a->audio, &a->cfg, &a->state) == 0);
  a->state.mode = MODE_FACE;
}
static void tests_config(void) {
  Config c;
  config_defaults(&c);
  CHECK(c.ring_front == 6);
  CHECK(c.frame_ms == 500);
  CHECK(config_set(&c, "brightness", "256") < 0);
  CHECK(config_set(&c, "ring_front", "12") < 0);
  CHECK(config_set(&c, "ring_color", "XYZXYZ") < 0);
  CHECK(config_set(&c, "actions", "582,148") == 0);
  CHECK(c.action_count == 2);
  CHECK(config_set(&c, "actions", "582,582") < 0);
  CHECK(config_set(&c, "actions", "582,garbage") < 0);
  CHECK(config_set(&c, "actions", "582,") < 0);
  CHECK(config_set(&c, "actions", "582junk") < 0);
  CHECK(config_set(&c, "actions", "582,148,115") < 0);
  CHECK(config_set(&c, "actions", "999999999999999999999999") < 0);
  CHECK(config_set(&c, "current_divider", "00") < 0);
  CHECK(config_set(&c, "current_divider", "+0") < 0);
  Config invalid = c;
  invalid.action_keys[0] = invalid.mute_key;
  CHECK(config_validate(&invalid) < 0);
  invalid = c;
  invalid.frame_ms = 0;
  CHECK(config_save(&invalid) < 0);
  CHECK(config_set(&c, "face", "5") == 0);
  CHECK(config_set(&c, "ring_color", "abcdef") == 0);
  CHECK(config_save(&c) == 0);
  Config loaded;
  CHECK(config_load(&loaded) == 0);
  CHECK(loaded.face == 5);
  CHECK(!strcmp(loaded.ring_color, "abcdef"));
  CHECK(loaded.action_count == 2);
  char p[MAXPATH];
  path_for(p, sizeof p, CONFIG_PATH);
  unlink(p);
  put("/etc/capsule-controls.json",
      "{\"brightness\":85,\"initial_mode\":\"clock\",\"action_keys\":[148],"
      "\"ring_front_center\":0,\"animation_frame_seconds\":0.75}");
  CHECK(config_load(&loaded) == 0);
  CHECK(loaded.brightness == 85);
  CHECK(loaded.initial_mode == MODE_CLOCK);
  CHECK(loaded.action_keys[0] == 148);
  CHECK(loaded.ring_front == 0);
  CHECK(loaded.frame_ms == 750);
  put("/etc/capsule-controls.json", "{\"mute_key\":99999}");
  CHECK(config_load(&loaded) < 0);
  put("/etc/capsule-controls.json", "{\"ring_front_center\":-999}");
  CHECK(config_load(&loaded) < 0);
  put("/etc/capsule-controls.json", "{\"current_divider\":0}");
  CHECK(config_load(&loaded) < 0);
  put("/etc/capsule-controls.json", "{}");
  State s = {65, 1, MODE_USD}, other = {0};
  CHECK(state_save(&s) == 0);
  CHECK(state_load(&other) == 0);
  CHECK(other.volume == 65 && other.muted == 1 && other.mode == MODE_USD);
  for (int v = -10; v <= 110; v++) {
    int n = volume_step(v);
    CHECK(n >= 0 && n <= 100 && n % 5 == 0);
  }
}
static void tests_rate(void) {
  const char *xml =
      "<?xml version=\"1.0\"?><ValCurs Date=\"24.10.2026\"><Valute "
      "ID=\"x\"><CharCode>EUR</CharCode><Nominal>1</Nominal><Value>99,0000</"
      "Value></Valute><Valute "
      "ID=\"y\"><CharCode>USD</CharCode><Nominal>10</Nominal><Value>934,5670</"
      "Value></Valute></ValCurs>";
  uint64_t micro = 0;
  char date[16];
  CHECK(rate_parse(xml, &micro, date) == 0);
  CHECK(micro == 93456700);
  CHECK(!strcmp(date, "24.10.2026"));
  CHECK(rate_parse(
            "<ValCurs "
            "Date=\"24.10.2026\"><Valute><CharCode>USD</CharCode><Nominal>0</"
            "Nominal><Value>80,5381</Value></Valute></ValCurs>",
            &micro, date) < 0);
  CHECK(rate_parse(
            "<ValCurs "
            "Date=\"24.10.2026\"><Valute><CharCode>USD</CharCode><Nominal>1</"
            "Nominal><Value>NaN</Value></Valute></ValCurs>",
            &micro, date) < 0);
  CHECK(rate_parse(
            "<ValCurs "
            "Date=\"24.10.2026\"><Valute><CharCode>USD</CharCode><Nominal>1</"
            "Nominal><Value>80,5381</Value></Valute></ValCurs>",
            &micro, date) == 0);
  CHECK(micro == 80538100);
  CHECK(rate_parse("no USD", &micro, date) < 0);
}
static void tests_ring(void) {
  Config c;
  config_defaults(&c);
  unsigned char frame[36];
  for (int v = 0; v <= 100; v += 5) {
    ring_frame(&c, v, true, FEEDBACK_VOLUME, frame);
    for (int i = 0; i < 12; i++) {
      CHECK(frame[i * 3] == (i >= 5 && i <= 7 ? 105 : 0));
      CHECK(frame[i * 3 + 1] == (i >= 5 && i <= 7 ? 6 : 0));
      CHECK(frame[i * 3 + 2] == 0);
    }
  }
  c.effect = FX_SOLID;
  ring_frame(&c, 0, false, FEEDBACK_VOLUME, frame);
  for (int i = 0; i < 36; i++)
    CHECK(frame[i] == 0);
  c.ring_front = 0;
  ring_frame(&c, 50, true, FEEDBACK_NONE, frame);
  for (int i = 0; i < 12; i++)
    CHECK(frame[i * 3] == (i == 0 || i == 1 || i == 11 ? 105 : 0));
  c.ring_front = 6;
  c.effect = FX_OFF;
  ring_frame(&c, 100, false, FEEDBACK_VOLUME, frame);
  for (int i = 0; i < 12; i++)
    CHECK(frame[i * 3] > 100);
}
static void tests_keys(void) {
  static App a;
  init_app(&a);
  key_event(&a, 248, 0, 99);
  key_event(&a, 582, 0, 99);
  CHECK(a.state.muted == 0 && a.state.mode == MODE_FACE);
  audio_close(&a.audio);
  for (int reverse = 0; reverse < 2; reverse++) {
    init_app(&a);
    int first = reverse ? 582 : 248, second = reverse ? 248 : 582;
    key_event(&a, first, 1, 100);
    CHECK(a.state.muted == 0);
    key_event(&a, second, 1, 100.1);
    CHECK(a.editing);
    CHECK(a.state.muted == 0);
    CHECK(a.state.mode == MODE_FACE);
    key_event(&a, first, 0, 100.2);
    key_event(&a, second, 0, 100.3);
    CHECK(a.editing);
    tap(&a, 115, 101);
    CHECK(a.cfg.effect == FX_RAINBOW);
    CHECK(a.state.volume == 50);
    tap(&a, 582, 102);
    CHECK(!a.editing);
    CHECK(a.config_dirty);
    CHECK(a.state.mode == MODE_FACE);
    CHECK(!a.state.muted);
    tap(&a, 248, 103);
    CHECK(a.state.muted);
    CHECK(a.overlay_until == 0);
    tap(&a, 248, 104);
    CHECK(!a.state.muted);
    tap(&a, 582, 105);
    CHECK(a.state.mode == MODE_USD);
    tap(&a, 115, 106);
    CHECK(a.state.volume == 55);
    audio_close(&a.audio);
  }
  init_app(&a);
  key_event(&a, 248, 1, 200);
  key_event(&a, 582, 1, 200.1);
  key_event(&a, 248, 0, 200.2);
  key_event(&a, 582, 0, 200.3);
  tap(&a, 114, 201);
  CHECK(a.cfg.effect == FX_SOLID);
  tap(&a, 248, 202);
  CHECK(!a.editing);
  CHECK(a.cfg.effect == FX_OFF);
  CHECK(!a.state.muted);
  for (int i = 0; i < 40; i++)
    tap(&a, 115, 210 + i);
  CHECK(a.state.volume == 100);
  for (int i = 0; i < 40; i++)
    tap(&a, 114, 260 + i);
  CHECK(a.state.volume == 0);
  audio_close(&a.audio);
}
static void tests_startup(void) {
  const char *original =
      "#!/bin/sh\nmpg123 /etc/kde_startup_sound.mp3 &> /dev/null &\nclockdisp "
      "--anim blink && clockdisp --anim angry && clockdisp --anim wink "
      "&\nringled off\n";
  put("/etc/local.d/rc.local.start", original);
  char p[MAXPATH], b[2048];
  path_for(p, sizeof p, "/etc/local.d/rc.local.start");
  chmod(p, 0755);
  CHECK(startup_sound(-1) == 1);
  CHECK(startup_prepare(true) == 0);
  CHECK(startup_sound(-1) == 0);
  CHECK(read_text(p, b, sizeof b) > 0);
  CHECK(strstr(b, "# capsule-controls replaced this display loop:") != NULL);
  CHECK(startup_sound(1) == 1);
  CHECK(startup_sound(-1) == 1);
  CHECK(startup_restore() >= 0);
  CHECK(read_text(p, b, sizeof b) > 0);
  CHECK(!strcmp(b, original));
  struct stat s;
  CHECK(stat(p, &s) == 0);
  CHECK((s.st_mode & 0777) == 0755);
}
int main(void) {
  char tmp[] = "/tmp/caps-native-core-XXXXXX";
  CHECK(mkdtemp(tmp) != NULL);
  root_prefix = tmp;
  tests_config();
  tests_rate();
  tests_ring();
  tests_keys();
  tests_startup();
  char *args[] = {"rm", "-rf", tmp, NULL};
  CHECK(run_cmd(args, 3) == 0);
  printf("Native core: %u assertions passed\n", assertions);
  return 0;
}
