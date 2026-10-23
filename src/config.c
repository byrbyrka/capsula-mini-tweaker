#include "capsule.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *effect_names[FX_COUNT] = {"выключено", "радуга", "огонь",
                                      "постоянный цвет"};
const char *face_names[6] = {"wink", "blink", "sleep", "wow", "angry", "idle"};
int volume_step(int v) {
  if (v < 0)
    v = 0;
  if (v > 100)
    v = 100;
  return ((v + 2) / 5) * 5;
}
void config_defaults(Config *c) {
  memset(c, 0, sizeof *c);
  strcpy(c->input, "auto");
  strcpy(c->timezone, "Europe/Moscow");
  strcpy(c->card, "0");
  strcpy(c->volume_control, "Master Playback Volume");
  strcpy(c->mute_control, "Playback Switch");
  strcpy(c->ringled, "/usr/local/bin/ringled");
  strcpy(c->ring_color, "00ffdb");
  c->up_key = 115;
  c->down_key = 114;
  c->mute_key = 248;
  c->action_keys[0] = 582;
  c->action_count = 1;
  c->grab = 1;
  c->initial_mode = MODE_FACE;
  c->brightness = -1;
  c->current_divider = -1;
  c->frame_ms = 500;
  c->ring_brightness = 50;
  c->ring_ms = 1500;
  c->ring_front = 6;
  c->ring_speed = 3000;
}
#define SET_INT(name, field, lo, hi)                                           \
  if (!strcmp(key, name)) {                                                    \
    if (parse_int(value, &n) || n < (lo) || n > (hi))                          \
      return -1;                                                               \
    c->field = n;                                                              \
    return 0;                                                                  \
  }
#define SET_STR(name, field)                                                   \
  if (!strcmp(key, name)) {                                                    \
    if (strlen(value) >= sizeof c->field || strchr(value, '\n'))               \
      return -1;                                                               \
    strcpy(c->field, value);                                                   \
    return 0;                                                                  \
  }
int config_set(Config *c, const char *key, const char *value) {
  int n;
  if (!strcmp(key, "current_divider")) {
    if (parse_int(value, &n) || (n != -1 && (n < 1 || n > 4)))
      return -1;
    c->current_divider = n;
    return 0;
  }
  SET_INT("brightness", brightness, -1, 255)
  SET_INT("frame_ms", frame_ms, 100, 5000)
  SET_INT("face", face, 0, 5)
  SET_INT("ring_brightness", ring_brightness, 0, 100)
  SET_INT("ring_ms", ring_ms, 100, 5000) SET_INT("ring_front", ring_front, 0,
                                                 11)
      SET_INT("effect", effect, 0, FX_COUNT + MAX_PRESETS - 1)
          SET_INT("ring_speed", ring_speed, 500, 30000) SET_INT(
              "up_key", up_key, 0, 767) SET_INT("down_key", down_key, 0, 767)
              SET_INT("mute_key", mute_key, 0, 767) SET_INT("grab", grab, 0, 1)
                  SET_INT("initial_mode", initial_mode, 0, 2)
                      SET_STR("input", input) SET_STR("timezone", timezone)
                          SET_STR("card", card)
                              SET_STR("volume_control", volume_control)
                                  SET_STR("mute_control", mute_control) SET_STR(
                                      "ringled",
                                      ringled) if (!strcmp(key, "actions")) {
    int a, b = 0;
    char codes[32];
    if (strlen(value) >= sizeof codes)
      return -1;
    strcpy(codes, value);
    char *comma = strchr(codes, ',');
    int count = comma ? 2 : 1;
    if (comma)
      *comma++ = 0;
    if (parse_int(codes, &a) || (comma && parse_int(comma, &b)) || a < 0 ||
        a >= KEY_COUNT || (count == 2 && (b < 0 || b >= KEY_COUNT || b == a)))
      return -1;
    c->action_keys[0] = a;
    c->action_count = count;
    c->action_keys[1] = b;
    return 0;
  }
  if (!strncmp(key, "preset", 6)) {
    int index;
    RingPreset preset;
    if (parse_int(key + 6, &index) || preset_parse(&preset, value))
      return -1;
    return preset_put(c, index, &preset);
  }
  if (!strcmp(key, "ring_color")) {
    if (strlen(value) != 6)
      return -1;
    for (int i = 0; i < 6; i++)
      if (!isxdigit((unsigned char)value[i]))
        return -1;
    strcpy(c->ring_color, value);
    return 0;
  }
  return -1;
}
int config_validate(const Config *c) {
#define RANGE(field, lo, hi)                                                   \
  if (c->field < (lo) || c->field > (hi))                                      \
  return -1
  RANGE(brightness, -1, 255);
  RANGE(current_divider, -1, 4);
  if (c->current_divider == 0)
    return -1;
  RANGE(frame_ms, 100, 5000);
  RANGE(face, 0, 5);
  RANGE(initial_mode, 0, 2);
  RANGE(ring_brightness, 0, 100);
  RANGE(ring_ms, 100, 5000);
  RANGE(ring_front, 0, 11);
  RANGE(ring_speed, 500, 30000);
  RANGE(up_key, 0, KEY_COUNT - 1);
  RANGE(down_key, 0, KEY_COUNT - 1);
  RANGE(mute_key, 0, KEY_COUNT - 1);
  RANGE(action_count, 1, 2);
  RANGE(grab, 0, 1);
#undef RANGE
  if (c->up_key == c->down_key || c->up_key == c->mute_key ||
      c->down_key == c->mute_key)
    return -1;
  for (int i = 0; i < c->action_count; i++)
    if (c->action_keys[i] < 0 || c->action_keys[i] >= KEY_COUNT ||
        c->action_keys[i] == c->up_key || c->action_keys[i] == c->down_key ||
        c->action_keys[i] == c->mute_key ||
        (i == 1 && c->action_keys[0] == c->action_keys[1]))
      return -1;
  return presets_validate(c);
}
static void migrate(Config *c) {
  char path[MAXPATH], old[8192], mode[32];
  path_for(path, sizeof path, "/etc/capsule-controls.json");
  if (read_text(path, old, sizeof old) < 0)
    return;
  c->brightness = json_int(old, "brightness", -1);
  c->current_divider = json_int(old, "current_divider", -1);
  c->ring_brightness = json_int(old, "ring_brightness", 50);
  c->ring_front = json_int(old, "ring_front_center", 6);
  c->up_key = json_int(old, "volume_up_key", 115);
  c->down_key = json_int(old, "volume_down_key", 114);
  c->mute_key = json_int(old, "mute_key", 248);
  c->grab = json_int(old, "grab_input", 1);
  json_string(old, "input_device", c->input, sizeof c->input);
  json_string(old, "timezone", c->timezone, sizeof c->timezone);
  if (!json_string(old, "initial_mode", mode, sizeof mode))
    c->initial_mode = !strcmp(mode, "usd") ? 0 : !strcmp(mode, "clock") ? 1 : 2;
  const char *p = strstr(old, "\"action_keys\"");
  if (p && (p = strchr(p, '['))) {
    int a, b;
    int n = sscanf(p + 1, " %d , %d", &a, &b);
    if (n > 0 && a >= 0 && a < KEY_COUNT) {
      c->action_keys[0] = a;
      c->action_count = 1;
      if (n == 2 && b >= 0 && b < KEY_COUNT && b != a) {
        c->action_keys[1] = b;
        c->action_count = 2;
      }
    }
  }
  p = strstr(old, "\"animation_frame_seconds\"");
  if (p && (p = strchr(p, ':'))) {
    double n = strtod(p + 1, NULL);
    if (n >= .1 && n <= 5)
      c->frame_ms = (int)(n * 1000 + .5);
  }
  p = strstr(old, "\"ring_feedback_seconds\"");
  if (p && (p = strchr(p, ':'))) {
    double n = strtod(p + 1, NULL);
    if (n >= .1 && n <= 5)
      c->ring_ms = (int)(n * 1000 + .5);
  }
}
int config_load(Config *c) {
  config_defaults(c);
  char path[MAXPATH];
  if (config_override)
    snprintf(path, sizeof path, "%s", config_override);
  else
    path_for(path, sizeof path, CONFIG_PATH);
  FILE *f = fopen(path, "r");
  if (!f) {
    if (errno != ENOENT)
      return -1;
    migrate(c);
    return config_validate(c);
  }
  char line[1024];
  int ok = 0;
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (isspace((unsigned char)*p))
      p++;
    if (*p == '#' || !*p)
      continue;
    char *eq = strchr(p, '=');
    if (!eq) {
      ok = -1;
      break;
    }
    *eq++ = 0;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1]))
      *--end = 0;
    while (isspace((unsigned char)*eq))
      eq++;
    end = eq + strlen(eq);
    while (end > eq && isspace((unsigned char)end[-1]))
      *--end = 0;
    if (config_set(c, p, eq)) {
      ok = -1;
      break;
    }
  }
  if (ferror(f))
    ok = -1;
  fclose(f);
  return ok ? ok : config_validate(c);
}
int config_save(const Config *c) {
  if (config_validate(c))
    return -1;
  char data[3200 + MAX_PRESETS * PRESET_TEXT], path[MAXPATH], actions[40];
  snprintf(actions, sizeof actions, "%d", c->action_keys[0]);
  if (c->action_count == 2)
    snprintf(actions, sizeof actions, "%d,%d", c->action_keys[0],
             c->action_keys[1]);
  int length = snprintf(
      data, sizeof data,
      "# capsule-tweaker " VERSION
      "\ninput=%s\ntimezone=%s\ncard=%s\nvolume_control=%s\nmute_control=%"
      "s\nup_key=%d\ndown_key=%d\nmute_key=%d\nactions=%s\ngrab=%"
      "d\ninitial_mode=%d\nbrightness=%d\ncurrent_divider=%d\nframe_ms=%"
      "d\nface=%d\nringled=%s\nring_brightness=%d\nring_ms=%d\nring_front="
      "%d\neffect=%d\nring_speed=%d\nring_color=%s\n",
      c->input, c->timezone, c->card, c->volume_control, c->mute_control,
      c->up_key, c->down_key, c->mute_key, actions, c->grab, c->initial_mode,
      c->brightness, c->current_divider, c->frame_ms, c->face, c->ringled,
      c->ring_brightness, c->ring_ms, c->ring_front, c->effect, c->ring_speed,
      c->ring_color);
  if (length < 0 || (size_t)length >= sizeof data)
    return -1;
  size_t used = (size_t)length;
  for (int i = 0; i < c->preset_count; i++) {
    char record[PRESET_TEXT];
    if (preset_format(&c->presets[i], record, sizeof record))
      return -1;
    int n =
        snprintf(data + used, sizeof data - used, "preset%d=%s\n", i, record);
    if (n < 0 || (size_t)n >= sizeof data - used)
      return -1;
    used += (size_t)n;
  }
  if (config_override)
    snprintf(path, sizeof path, "%s", config_override);
  else
    path_for(path, sizeof path, CONFIG_PATH);
  return atomic_text(path, data);
}
int state_load(State *s) {
  char path[MAXPATH], data[512], mode[20];
  path_for(path, sizeof path, STATE_PATH);
  if (read_text(path, data, sizeof data) < 0)
    return -1;
  s->volume = volume_step(json_int(data, "volume", s->volume));
  s->muted = !!json_int(data, "muted", s->muted);
  if (!json_string(data, "mode", mode, sizeof mode))
    s->mode = !strcmp(mode, "usd") ? 0 : !strcmp(mode, "clock") ? 1 : 2;
  return 0;
}
int state_save(const State *s) {
  char path[MAXPATH], data[160];
  const char *names[] = {"usd", "clock", "face"};
  snprintf(data, sizeof data, "{\"volume\":%d,\"muted\":%s,\"mode\":\"%s\"}\n",
           s->volume, s->muted ? "true" : "false", names[s->mode]);
  path_for(path, sizeof path, STATE_PATH);
  return atomic_text(path, data);
}
