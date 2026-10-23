#include "capsule.h"
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool action(const Config *c, int code) {
  for (int i = 0; i < c->action_count; i++)
    if (code == c->action_keys[i])
      return true;
  return false;
}
int input_open(const Config *c) {
  char device[MAXPATH];
  if (strcmp(c->input, "auto"))
    path_for(device, sizeof device, c->input);
  else {
    device[0] = 0;
    char pattern[MAXPATH];
    path_for(pattern, sizeof pattern, "/sys/class/input/event*/device/name");
    glob_t g = {0};
    if (!glob(pattern, 0, NULL, &g))
      for (size_t i = 0; i < g.gl_pathc; i++) {
        char name[128];
        if (read_text(g.gl_pathv[i], name, sizeof name) > 0 &&
            (strstr(name, "CSK05T") || strstr(name, "csk05t"))) {
          const char *p = strstr(g.gl_pathv[i], "/event");
          if (p) {
            char event[24];
            if (sscanf(p + 1, "%23[^/]", event) == 1) {
              char abs[48];
              snprintf(abs, sizeof abs, "/dev/input/%s", event);
              path_for(device, sizeof device, abs);
              break;
            }
          }
        }
      }
    globfree(&g);
    if (!*device)
      return -1;
  }
  int fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    return -1;
  if (c->grab && ioctl(fd, _IOW('E', 0x90, int), 1) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}
void keys_drop(App *a) {
  memset(&a->keys, 0, sizeof a->keys);
  a->keys.dropped = true;
}
void key_event(App *a, int code, int value, double now) {
  if (code < 0 || code >= KEY_COUNT || value < 0 || value > 2)
    return;
  Keys *k = &a->keys;
  Config *c = &a->cfg;
  bool was_down = k->down[code];
  if ((value == 0 || value == 2) && !was_down)
    return; // Ignore stale releases/repeats after opening or resynchronising
            // input.
  if (value != 2)
    k->down[code] = value == 1;
  bool any_action = false;
  for (int i = 0; i < c->action_count; i++)
    any_action |= k->down[c->action_keys[i]];
  if (k->blocked) {
    if (!k->down[c->mute_key] && !any_action)
      k->blocked = false;
    return;
  }
  if (k->down[c->mute_key] && any_action && !k->chord) {
    k->chord = true;
    app_edit(a, now);
    return;
  }
  if (k->chord) {
    if (!k->down[c->mute_key] && !any_action)
      k->chord = false;
    return;
  }
  if (code == c->up_key || code == c->down_key) {
    if (!value || now - k->last_volume < .10)
      return;
    k->last_volume = now;
    int delta = code == c->up_key ? 1 : -1;
    if (a->editing)
      app_change_effect(
          a, (c->effect + delta + ring_effect_count(c)) % ring_effect_count(c),
          now);
    else
      app_update(a, volume_step(a->state.volume + delta * 5), 0, a->state.mode,
                 now);
    return;
  }
  // Single mute/action presses are applied on release, allowing a chord in
  // either order.
  if (value != 0)
    return;
  if (code == c->mute_key) {
    if (a->editing) {
      c->effect = a->old_effect;
      a->editing = false;
      a->ring.last = 0;
    } else
      app_update(a, a->state.volume, !a->state.muted, a->state.mode, now);
  } else if (action(c, code)) {
    if (a->editing) {
      a->editing = false;
      a->config_dirty = true;
      a->save_at = now + 2;
      a->ring.last = 0;
    } else
      app_update(a, a->state.volume, a->state.muted, (a->state.mode + 1) % 3,
                 now);
  }
}
