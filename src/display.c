#include "capsule.h"
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int first_glob(char *out, size_t cap, const char *pattern) {
  char full[MAXPATH];
  path_for(full, sizeof full, pattern);
  glob_t g = {0};
  int r = glob(full, 0, NULL, &g);
  if (!r && g.gl_pathc)
    snprintf(out, cap, "%s", g.gl_pathv[0]);
  globfree(&g);
  return r ? -1 : 0;
}
int display_open(Display *d) {
  memset(d, 0, sizeof *d);
  d->last_top = d->last_bottom = -1;
  d->configured_brightness = d->configured_current = -999;
  const char *patterns[] = {"/sys/class/linedisp/linedisp.*/message",
                            "/sys/bus/platform/devices/*/linedisp.*/message",
                            "/sys/devices/platform/*/linedisp.*/message"};
  for (unsigned i = 0; i < 3; i++)
    if (!first_glob(d->message, sizeof d->message, patterns[i]))
      break;
  if (!*d->message)
    return -1;
  snprintf(d->map, sizeof d->map, "%s", d->message);
  strcpy(strrchr(d->map, '/') + 1, "map_seg7");
  snprintf(d->brightness, sizeof d->brightness, "%.*s/../brightness",
           (int)(strrchr(d->message, '/') - d->message), d->message);
  first_glob(d->current, sizeof d->current,
             "/sys/bus/i2c/devices/*/current_divider");
  path_for(d->colon_top, sizeof d->colon_top,
           "/sys/class/leds/clock:colon:top/brightness");
  path_for(d->colon_bottom, sizeof d->colon_bottom,
           "/sys/class/leds/clock:colon:bottom/brightness");
  int fd = open(d->map, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, d->glyphs, sizeof d->glyphs);
  close(fd);
  if (n != 128)
    return -1;
  char p[MAXPATH];
  path_for(p, sizeof p, "/sys/class/leds/clock:colon:top/trigger");
  write_bytes(p, "none", 4);
  path_for(p, sizeof p, "/sys/class/leds/clock:colon:bottom/trigger");
  write_bytes(p, "none", 4);
  d->found = true;
  return 0;
}
static int number_file(const char *path, int value) {
  char text[16];
  int n = snprintf(text, sizeof text, "%d", value);
  return write_bytes(path, text, (size_t)n);
}
static int brightness(App *a) {
  if (a->cfg.brightness >= 0)
    return a->cfg.brightness;
  char text[32];
  if (read_text(a->display.brightness, text, sizeof text) > 0)
    return atoi(text);
  return 255;
}
static void show(App *a, const char message[4], int top, int bottom) {
  Display *d = &a->display;
  if (memcmp(d->last, message, 4)) {
    if (!write_bytes(d->message, message, 4))
      memcpy(d->last, message, 4);
  }
  if (top != d->last_top) {
    if (!number_file(d->colon_top, top))
      d->last_top = top;
  }
  if (bottom != d->last_bottom) {
    if (!number_file(d->colon_bottom, bottom))
      d->last_bottom = bottom;
  }
}
static void text4(char out[4], const char *text) {
  size_t n = strlen(text);
  if (n > 4)
    n = 4;
  memset(out, ' ', 4);
  memcpy(out + 4 - n, text, n);
}
static void face(App *a, double now) {
  static const unsigned char frames[6][3][2] = {
      {{0x23, 0x23}, {0x23, 0x40}, {0x23, 0x23}},
      {{0x23, 0x23}, {0x40, 0x40}, {0x23, 0x23}},
      {{0x23, 0x23}, {0x40, 0x40}, {0x08, 0x08}},
      {{0x40, 0x40}, {0x5c, 0x5c}, {0x3f, 0x3f}},
      {{0x40, 0x40}, {0x21, 0x03}, {0x21, 0x03}},
      {{0x23, 0x23}, {0x23, 0x23}, {0x23, 0x23}}};
  int count = a->cfg.face == 5 ? 1 : a->cfg.face == 4 ? 2 : 3;
  int i = (int)((uint64_t)(now * 1000) / a->cfg.frame_ms % (unsigned)count);
  Display *d = &a->display;
  unsigned char left = frames[a->cfg.face][i][0],
                right = frames[a->cfg.face][i][1];
  d->next = now + (a->cfg.frame_ms - (uint64_t)(now * 1000) % a->cfg.frame_ms) /
                      1000.0;
  if (d->glyphs[1] != left || d->glyphs[2] != right) {
    unsigned char updated[128];
    memcpy(updated, d->glyphs, sizeof updated);
    updated[1] = left;
    updated[2] = right;
    if (write_bytes(d->map, updated, sizeof updated))
      return;
    memcpy(d->glyphs, updated, sizeof updated);
    // map_seg7 changes the lookup table; message must be written again to
    // refresh the visible segments, even when its character codes are equal.
    memset(d->last, 0, sizeof d->last);
  }
  const char msg[4] = {' ', 1, 2, ' '};
  show(a, msg, 0, brightness(a));
}
void display_tick(App *a, double now) {
  Display *d = &a->display;
  if (!d->found) {
    if (now < d->retry)
      return;
    if (display_open(d)) {
      d->retry = now + 5;
      return;
    }
  }
  if (a->cfg.brightness != d->configured_brightness) {
    if (a->cfg.brightness < 0 || !number_file(d->brightness, a->cfg.brightness))
      d->configured_brightness = a->cfg.brightness;
    d->last_top = d->last_bottom = -1;
  }
  if (a->cfg.current_divider != d->configured_current) {
    if (a->cfg.current_divider < 0 ||
        (*d->current && !number_file(d->current, a->cfg.current_divider)))
      d->configured_current = a->cfg.current_divider;
  }
  char text[32], msg[4];
  int top = 0, bottom = 0;
  d->next = now + 10;
  if (a->editing)
    snprintf(text, sizeof text, "r %d", a->cfg.effect);
  else if (now < a->overlay_until) {
    snprintf(text, sizeof text, "%s", a->overlay);
    d->next = a->overlay_until;
  } else if (a->state.mode == MODE_FACE) {
    face(a, now);
    return;
  } else if (a->state.mode == MODE_CLOCK) {
    time_t t = time(NULL);
    struct tm local;
    localtime_r(&t, &local);
    strftime(text, sizeof text, "%H%M", &local);
    top = bottom = brightness(a);
    d->next = now + (60 - local.tm_sec);
  } else if (a->rate.micro) {
    uint64_t cents = (a->rate.micro + 5000) / 10000;
    if (cents < 10000) {
      snprintf(text, sizeof text, "%04u", (unsigned)cents);
      bottom = brightness(a);
    } else
      snprintf(text, sizeof text, "%4u", (unsigned)((cents + 50) / 100));
  } else
    strcpy(text, "----");
  text4(msg, text);
  show(a, msg, top, bottom);
}
void display_close(Display *d) {
  if (!d->found)
    return;
  write_bytes(d->message, "    ", 4);
  number_file(d->colon_top, 0);
  number_file(d->colon_bottom, 0);
}
