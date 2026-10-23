#include "capsule.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned assertions, message_writes, map_writes;
static bool tracking, fail_map, fail_message, fail_brightness, fail_bottom;
static char message_path[MAXPATH], map_path[MAXPATH];
static unsigned char hardware_map[128], visible[4];
#define CHECK(x)                                                               \
  do {                                                                         \
    assert(x);                                                                 \
    assertions++;                                                              \
  } while (0)

ssize_t __real_write(int fd, const void *data, size_t count);
ssize_t __wrap_write(int fd, const void *data, size_t count) {
  char link[64], path[MAXPATH];
  snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  ssize_t length = readlink(link, path, sizeof path - 1);
  if (length < 0)
    return __real_write(fd, data, count);
  path[length] = 0;
  bool map = tracking && !strcmp(path, map_path);
  bool message = tracking && !strcmp(path, message_path);
  bool bottom = tracking && strstr(path, "clock:colon:bottom/brightness");
  bool brightness = tracking && strstr(path, "/display/brightness");
  if ((map && fail_map) || (message && fail_message) ||
      (brightness && fail_brightness) || (bottom && fail_bottom)) {
    fail_map = fail_message = fail_brightness = fail_bottom = false;
    errno = EIO;
    return -1;
  }
  ssize_t result = __real_write(fd, data, count);
  if (result != (ssize_t)count)
    return result;
  /* Model latched display hardware: writing map_seg7 changes the lookup
   * table, but visible segments refresh only when message is written. */
  if (map && count == 128) {
    memcpy(hardware_map, data, 128);
    map_writes++;
  }
  if (message && count == 4) {
    const unsigned char *characters = data;
    for (int i = 0; i < 4; i++)
      visible[i] = hardware_map[characters[i] & 127];
    message_writes++;
  }
  return result;
}
static void put(const char *name, const char *text) {
  char path[MAXPATH];
  path_for(path, sizeof path, name);
  CHECK(atomic_text(path, text) == 0);
}
static void test_faces(void) {
  App a = {0};
  config_defaults(&a.cfg);
  a.state.mode = MODE_FACE;
  a.cfg.brightness = 100;
  CHECK(display_open(&a.display) == 0);
  tracking = true;

  display_tick(&a, 150.1); // wink: idle
  CHECK(visible[1] == 0x23 && visible[2] == 0x23);
  CHECK(map_writes == 1 && message_writes == 1);
  display_tick(&a, 150.6); // wink: right eye closed
  CHECK(visible[1] == 0x23 && visible[2] == 0x40);
  CHECK(map_writes == 2 && message_writes == 2);
  display_tick(&a, 151.1); // wink: idle
  CHECK(visible[1] == 0x23 && visible[2] == 0x23);
  CHECK(map_writes == 3 && message_writes == 3);
  display_tick(&a, 151.6); // repeated idle frame does not need an I/O write
  CHECK(map_writes == 3 && message_writes == 3);

  for (int face = 0; face < 6; face++) {
    a.cfg.face = face;
    int count = face == 5 ? 1 : face == 4 ? 2 : 3;
    static const unsigned char expected[6][3][2] = {
        {{0x23, 0x23}, {0x23, 0x40}, {0x23, 0x23}},
        {{0x23, 0x23}, {0x40, 0x40}, {0x23, 0x23}},
        {{0x23, 0x23}, {0x40, 0x40}, {0x08, 0x08}},
        {{0x40, 0x40}, {0x5c, 0x5c}, {0x3f, 0x3f}},
        {{0x40, 0x40}, {0x21, 0x03}, {0x21, 0x03}},
        {{0x23, 0x23}, {0x23, 0x23}, {0x23, 0x23}}};
    for (int f = 0; f < count; f++) {
      /* 150 seconds is divisible by all animation cycle lengths. */
      double now = 150 + f * .5 + .1;
      display_tick(&a, now);
      CHECK(visible[1] == expected[face][f][0]);
      CHECK(visible[2] == expected[face][f][1]);
      CHECK(visible[0] == 0 && visible[3] == 0);
      CHECK(a.display.next > now && a.display.next <= now + .5);
    }
  }
  unsigned last_message = message_writes, last_map = map_writes;
  for (int i = 0; i < 20; i++)
    display_tick(&a, 200 + i * .5);
  CHECK(message_writes == last_message && map_writes == last_map);

  a.cfg.face = 0;
  fail_map = true;
  display_tick(&a, 150.6);
  CHECK(visible[2] == 0x23); // failed map write must not commit the cache
  display_tick(&a, 150.7);
  CHECK(visible[2] == 0x40);

  fail_message = true;
  display_tick(&a, 151.1);
  CHECK(visible[2] == 0x40);
  display_tick(&a, 151.2);
  CHECK(visible[2] == 0x23); // failed message write must also be retried

  /* An overlay restores the face even if the cached segment map is identical.
   */
  strcpy(a.overlay, "55");
  a.overlay_until = 151.3;
  display_tick(&a, 151.25);
  CHECK(visible[1] == 0 && visible[2] == 0x6d && visible[3] == 0x6d);
  display_tick(&a, 151.4);
  CHECK(visible[1] == 0x23 && visible[2] == 0x23);
  fail_brightness = true;
  a.cfg.brightness = 80;
  display_tick(&a, 151.45);
  CHECK(a.display.configured_brightness == 100);
  display_tick(&a, 151.46);
  CHECK(a.display.configured_brightness == 80);
  fail_bottom = true;
  a.cfg.brightness = 60;
  display_tick(&a, 151.47);
  CHECK(a.display.last_bottom != 60);
  display_tick(&a, 151.48);
  CHECK(a.display.last_bottom == 60);
  display_close(&a.display);
  CHECK(!memcmp(visible, (unsigned char[4]){0}, 4));
  tracking = false;
}
int main(void) {
  char tmp[] = "/tmp/caps-display-XXXXXX";
  CHECK(mkdtemp(tmp) != NULL);
  root_prefix = tmp;
  const char *base = "/sys/bus/platform/devices/display/linedisp.0";
  char path[MAXPATH];
  path_for(path, sizeof path, base);
  CHECK(mkdirs(path) == 0);
  snprintf(message_path, sizeof message_path, "%s%s/message", tmp, base);
  snprintf(map_path, sizeof map_path, "%s%s/map_seg7", tmp, base);
  CHECK(atomic_text(message_path, "    ") == 0);
  hardware_map['5'] = 0x6d;
  CHECK(atomic_text(map_path, "") == 0);
  CHECK(write_bytes(map_path, hardware_map, 128) == 0);
  put("/sys/bus/platform/devices/display/brightness", "255");
  for (int i = 0; i < 2; i++) {
    char name[100];
    snprintf(name, sizeof name, "/sys/class/leds/clock:colon:%s/brightness",
             i ? "bottom" : "top");
    put(name, "0");
    snprintf(name, sizeof name, "/sys/class/leds/clock:colon:%s/trigger",
             i ? "bottom" : "top");
    put(name, "none");
  }
  test_faces();
  char *args[] = {"rm", "-rf", tmp, NULL};
  CHECK(run_cmd(args, 3) == 0);
  printf("Native display: %u assertions passed\n", assertions);
  return 0;
}
