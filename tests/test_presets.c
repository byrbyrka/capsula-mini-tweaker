#include "capsule.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x)                                                               \
  do {                                                                         \
    assert(x);                                                                 \
    assertions++;                                                              \
  } while (0)

static RingPreset sample(const char *name) {
  RingPreset p = {.effect = FX_FRAMES,
                  .brightness = 37,
                  .speed = 2500,
                  .frame_ms = 250,
                  .count = MAX_FRAMES};
  snprintf(p.name, sizeof p.name, "%s", name);
  for (int f = 0; f < p.count; f++)
    for (int i = 0; i < 36; i++)
      p.frames[f][i] = (unsigned char)(f * 17 + i);
  return p;
}
static void tests_records(void) {
  int value;
  CHECK(parse_int(" ", &value) < 0);
  RingPreset p = sample("Мой огонёк"), parsed;
  char text[PRESET_TEXT], small[4];
  CHECK(preset_format(&p, text, sizeof text) == 0);
  CHECK(preset_format(&p, small, sizeof small) < 0);
  CHECK(preset_parse(&parsed, text) == 0);
  CHECK(!memcmp(&p, &parsed, sizeof p));
  size_t n = strlen(text);
  for (size_t i = 0; i < n; i++) {
    char truncated[PRESET_TEXT];
    memcpy(truncated, text, i);
    truncated[i] = 0;
    /* Truncation exactly at a complete frame is a valid shorter preset. */
    if (preset_parse(&parsed, truncated) == 0) {
      CHECK(parsed.count < p.count);
      CHECK(!memcmp(parsed.frames, p.frames, (size_t)parsed.count * 36));
    } else
      CHECK(1);
  }
  parsed = p;
  CHECK(preset_parse(&parsed, "name|4|50|3000|250|GG") < 0);
  CHECK(!memcmp(&p, &parsed, sizeof p));
  p.name[0] = 0;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p = sample(" space");
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p = sample("pipe|name");
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p = sample("newline\nname");
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p = sample("valid");
  p.brightness = 101;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p.brightness = 0;
  p.frame_ms = 249;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p.frame_ms = 10001;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p.frame_ms = 250;
  p.count = 0;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p.count = MAX_FRAMES + 1;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p.count = MAX_FRAMES;
  p.effect = FX_FRAMES + 1;
  CHECK(preset_format(&p, text, sizeof text) < 0);
  p.effect = FX_FRAMES;
  memset(p.name, 'a', sizeof p.name - 1);
  p.name[sizeof p.name - 1] = 0;
  CHECK(preset_format(&p, text, sizeof text) == 0);
  CHECK(preset_parse(&parsed, text) == 0);
  char bad[PRESET_TEXT + 3];
  snprintf(bad, sizeof bad, "%s|x", text);
  CHECK(preset_parse(&parsed, bad) < 0);
  text[strlen(text) - 1] = 'z';
  CHECK(preset_parse(&parsed, text) < 0);
}
static void tests_store(void) {
  Config c, loaded;
  config_defaults(&c);
  for (int i = 0; i < MAX_PRESETS; i++) {
    char name[30];
    snprintf(name, sizeof name, "Пресет %d", i);
    RingPreset p = sample(name);
    CHECK(preset_put(&c, i, &p) == 0);
  }
  CHECK(ring_effect_count(&c) == FX_COUNT + MAX_PRESETS);
  RingPreset p = sample("extra");
  CHECK(preset_put(&c, MAX_PRESETS, &p) < 0);
  CHECK(preset_put(&c, -1, &p) < 0);
  CHECK(preset_put(&c, 0, &c.presets[1]) < 0);
  c.effect = FX_COUNT + MAX_PRESETS - 1;
  CHECK(config_validate(&c) == 0);
  CHECK(config_save(&c) == 0);
  CHECK(config_load(&loaded) == 0);
  CHECK(!memcmp(&c, &loaded, sizeof c));
  CHECK(!strcmp(ring_effect_name(&loaded, loaded.effect), "Пресет 15"));
  CHECK(!strcmp(ring_effect_name(&loaded, FX_FIRE), effect_names[FX_FIRE]));
  CHECK(preset_delete(&c, -1) < 0);
  CHECK(preset_delete(&c, MAX_PRESETS) < 0);
  CHECK(preset_delete(&c, 0) == 0);
  CHECK(c.effect == FX_COUNT + MAX_PRESETS - 2);
  CHECK(!strcmp(ring_effect_name(&c, c.effect), "Пресет 15"));
  CHECK(preset_delete(&c, c.preset_count - 1) == 0);
  CHECK(c.effect == FX_OFF);
  CHECK(config_save(&c) == 0);
  CHECK(config_load(&loaded) == 0);
  CHECK(!memcmp(&c, &loaded, sizeof c));
  c.effect = ring_effect_count(&c);
  CHECK(config_validate(&c) < 0);
  CHECK(config_save(&c) < 0);
  char path[MAXPATH];
  path_for(path, sizeof path, CONFIG_PATH);
  CHECK(atomic_text(path, "effect=4\n") == 0);
  CHECK(config_load(&loaded) < 0);
  CHECK(atomic_text(path, "effect=3\nring_color=ff8800\n") == 0);
  CHECK(config_load(&loaded) == 0);
  CHECK(loaded.preset_count == 0 && loaded.effect == FX_SOLID);
}
static void tests_frames(void) {
  RingPreset p = sample("Frames");
  unsigned char frame[36];
  for (int i = 0; i < MAX_FRAMES * 3; i++) {
    preset_frame(&p, i * .25, frame);
    CHECK(!memcmp(frame, p.frames[i % MAX_FRAMES], 36));
    preset_frame(&p, i * .25 + .249, frame);
    CHECK(!memcmp(frame, p.frames[i % MAX_FRAMES], 36));
  }
  preset_frame(&p, -1, frame);
  CHECK(!memcmp(frame, p.frames[0], 36));
  Config c;
  config_defaults(&c);
  CHECK(preset_put(&c, 0, &p) == 0);
  c.effect = FX_COUNT;
  ring_frame(&c, 50, false, FEEDBACK_NONE, frame);
  CHECK(!memcmp(frame, p.frames[0], 36));
  c.ring_front = 0;
  ring_frame(&c, 50, false, FEEDBACK_NONE, frame);
  for (int i = 0; i < 12; i++)
    CHECK(!memcmp(frame + i * 3, p.frames[0] + ((i + 6) % 12) * 3, 3));
  c.ring_front = 6;
  ring_frame(&c, 50, true, FEEDBACK_NONE, frame);
  for (int i = 0; i < 12; i++)
    CHECK(frame[i * 3] == (i >= 5 && i <= 7 ? 105 : 0));
  ring_frame(&c, 0, false, FEEDBACK_VOLUME, frame);
  CHECK(!memcmp(frame, (unsigned char[36]){0}, 36));
}
static void tap(App *a, int code, double now) {
  key_event(a, code, 1, now);
  key_event(a, code, 0, now + .01);
}
static void tests_controller(void) {
  App a = {0};
  config_defaults(&a.cfg);
  RingPreset p = sample("One");
  CHECK(preset_put(&a.cfg, 0, &p) == 0);
  p = sample("Two");
  CHECK(preset_put(&a.cfg, 1, &p) == 0);
  a.cfg.effect = FX_SOLID;
  a.state = (State){55, 1, MODE_FACE};
  app_edit(&a, 1);
  tap(&a, 115, 2);
  CHECK(a.cfg.effect == FX_COUNT && a.editing);
  CHECK(a.state.volume == 55 && a.state.muted == 1);
  tap(&a, 115, 3);
  CHECK(a.cfg.effect == FX_COUNT + 1);
  tap(&a, 115, 4);
  CHECK(a.cfg.effect == FX_OFF);
  tap(&a, 114, 5);
  CHECK(a.cfg.effect == FX_COUNT + 1);
  tap(&a, 248, 6);
  CHECK(!a.editing && a.cfg.effect == FX_SOLID);
  app_edit(&a, 7);
  tap(&a, 115, 8);
  tap(&a, 582, 9);
  CHECK(!a.editing && a.cfg.effect == FX_COUNT && a.config_dirty);
  char reply[PRESET_TEXT + 80], record[PRESET_TEXT], command[PRESET_TEXT + 80];
  CHECK(preset_ipc(&a, "PRESETS", reply, sizeof reply, 10) == 0);
  CHECK(!strcmp(reply, "OK 2\n4 One\n5 Two\n"));
  CHECK(preset_ipc(&a, "PRESET_GET 0", reply, sizeof reply, 10) == 0);
  RingPreset parsed;
  CHECK(preset_parse(&parsed, reply + 3) == 0);
  CHECK(!strcmp(parsed.name, "One"));
  CHECK(preset_ipc(&a, "PRESET_GET 2", reply, sizeof reply, 10) < 0);
  CHECK(preset_ipc(&a, "PRESET_DEL 0 x", reply, sizeof reply, 10) < 0);
  p = sample("Updated");
  CHECK(preset_format(&p, record, sizeof record) == 0);
  snprintf(command, sizeof command, "PRESET_PUT 0 %s", record);
  a.editing = true;
  CHECK(preset_ipc(&a, command, reply, sizeof reply, 10) < 0);
  CHECK(preset_ipc(&a, "PRESET_DEL 0", reply, sizeof reply, 10) < 0);
  a.editing = false;
  config_override = "/dev/null/capsule.conf";
  CHECK(preset_ipc(&a, command, reply, sizeof reply, 10) < 0);
  CHECK(!strcmp(a.cfg.presets[0].name, "One"));
  config_override = NULL;
  CHECK(preset_ipc(&a, command, reply, sizeof reply, 10) == 0);
  Config loaded;
  CHECK(config_load(&loaded) == 0);
  CHECK(!strcmp(loaded.presets[0].name, "Updated"));
  CHECK(loaded.effect == FX_COUNT);
  CHECK(preset_ipc(&a, "PRESET_DEL 0", reply, sizeof reply, 10) == 0);
  CHECK(a.cfg.effect == FX_OFF);
  CHECK(config_load(&loaded) == 0);
  CHECK(loaded.preset_count == 1 && !strcmp(loaded.presets[0].name, "Two"));
}
int main(void) {
  char tmp[] = "/tmp/caps-presets-XXXXXX";
  CHECK(mkdtemp(tmp) != NULL);
  root_prefix = tmp;
  tests_records();
  tests_store();
  tests_frames();
  tests_controller();
  char *args[] = {"rm", "-rf", tmp, NULL};
  CHECK(run_cmd(args, 3) == 0);
  printf("Native presets: %u assertions passed\n", assertions);
  return 0;
}
