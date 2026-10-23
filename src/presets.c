#include "capsule.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static int valid(const RingPreset *p) {
  size_t n = strlen(p->name);
  if (!n || n >= sizeof p->name || isspace((unsigned char)p->name[0]) ||
      isspace((unsigned char)p->name[n - 1]))
    return -1;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)p->name[i] < 32 || p->name[i] == 127 ||
        p->name[i] == '|')
      return -1;
  return p->effect < FX_OFF || p->effect > FX_FRAMES || p->brightness < 0 ||
                 p->brightness > 100 || p->speed < 500 || p->speed > 30000 ||
                 p->frame_ms < 250 || p->frame_ms > 10000 || p->count < 1 ||
                 p->count > MAX_FRAMES
             ? -1
             : 0;
}
int preset_parse(RingPreset *p, const char *text) {
  if (strlen(text) >= PRESET_TEXT)
    return -1;
  char buffer[PRESET_TEXT];
  strcpy(buffer, text);
  char *rest = buffer, *fields[6];
  for (int i = 0; i < 6; i++) {
    fields[i] = strsep(&rest, "|");
    if (!fields[i])
      return -1;
  }
  if (rest || strlen(fields[0]) >= sizeof p->name)
    return -1;
  RingPreset candidate = {0};
  strcpy(candidate.name, fields[0]);
  if (parse_int(fields[1], &candidate.effect) ||
      parse_int(fields[2], &candidate.brightness) ||
      parse_int(fields[3], &candidate.speed) ||
      parse_int(fields[4], &candidate.frame_ms))
    return -1;
  rest = fields[5];
  do {
    char *frame = strsep(&rest, ",");
    if (candidate.count == MAX_FRAMES || strlen(frame) != 72)
      return -1;
    for (int i = 0; i < 36; i++) {
      int a = hex_digit((unsigned char)frame[i * 2]);
      int b = hex_digit((unsigned char)frame[i * 2 + 1]);
      if (a < 0 || b < 0)
        return -1;
      candidate.frames[candidate.count][i] = (unsigned char)(a * 16 + b);
    }
    candidate.count++;
  } while (rest);
  if (valid(&candidate))
    return -1;
  *p = candidate;
  return 0;
}
int preset_format(const RingPreset *p, char *text, size_t cap) {
  if (valid(p))
    return -1;
  int n = snprintf(text, cap, "%s|%d|%d|%d|%d|", p->name, p->effect,
                   p->brightness, p->speed, p->frame_ms);
  if (n < 0 || (size_t)n >= cap)
    return -1;
  size_t used = (size_t)n;
  const char hex[] = "0123456789abcdef";
  for (int f = 0; f < p->count; f++) {
    if (cap - used < (size_t)(73 + (f != 0)))
      return -1;
    if (f)
      text[used++] = ',';
    for (int i = 0; i < 36; i++) {
      text[used++] = hex[p->frames[f][i] >> 4];
      text[used++] = hex[p->frames[f][i] & 15];
    }
  }
  text[used] = 0;
  return 0;
}
int ring_effect_count(const Config *c) { return FX_COUNT + c->preset_count; }
const char *ring_effect_name(const Config *c, int effect) {
  if (effect < 0 || effect >= ring_effect_count(c))
    return "свой пресет";
  return effect < FX_COUNT ? effect_names[effect]
                           : c->presets[effect - FX_COUNT].name;
}
int presets_validate(const Config *c) {
  if (c->preset_count < 0 || c->preset_count > MAX_PRESETS || c->effect < 0 ||
      c->effect >= ring_effect_count(c))
    return -1;
  for (int i = 0; i < c->preset_count; i++) {
    if (valid(&c->presets[i]))
      return -1;
    for (int j = 0; j < i; j++)
      if (!strcmp(c->presets[i].name, c->presets[j].name))
        return -1;
  }
  return 0;
}
int preset_put(Config *c, int index, const RingPreset *p) {
  if (index < 0 || index > c->preset_count || index >= MAX_PRESETS || valid(p))
    return -1;
  for (int i = 0; i < c->preset_count; i++)
    if (i != index && !strcmp(c->presets[i].name, p->name))
      return -1;
  c->presets[index] = *p;
  if (index == c->preset_count)
    c->preset_count++;
  return 0;
}
int preset_delete(Config *c, int index) {
  if (index < 0 || index >= c->preset_count)
    return -1;
  int effect = FX_COUNT + index;
  if (c->effect == effect)
    c->effect = FX_OFF;
  else if (c->effect > effect)
    c->effect--;
  memmove(&c->presets[index], &c->presets[index + 1],
          (size_t)(c->preset_count - index - 1) * sizeof c->presets[0]);
  memset(&c->presets[--c->preset_count], 0, sizeof c->presets[0]);
  return 0;
}
void preset_frame(const RingPreset *p, double elapsed, unsigned char out[36]) {
  if (elapsed < 0)
    elapsed = 0;
  unsigned long long frame = (unsigned long long)(elapsed * 1000 / p->frame_ms);
  memcpy(out, p->frames[frame % (unsigned)p->count], 36);
}

/* Mutations are saved atomically before acknowledging them. Button previews
 * cannot race preset edits: finish or cancel the button editor first. */
int preset_ipc(App *a, const char *request, char *reply, size_t cap,
               double now) {
  int index, used = 0;
  char extra;
  if (!strcmp(request, "PRESETS")) {
    size_t n = (size_t)snprintf(reply, cap, "OK %d\n", a->cfg.preset_count);
    for (int i = 0; i < a->cfg.preset_count && n < cap; i++)
      n += (size_t)snprintf(reply + n, cap - n, "%d %s\n", FX_COUNT + i,
                            a->cfg.presets[i].name);
    return 0;
  }
  if (sscanf(request, "PRESET_GET %d %c", &index, &extra) == 1) {
    if (index < 0 || index >= a->cfg.preset_count || cap < 4)
      return -1;
    memcpy(reply, "OK ", 3);
    return preset_format(&a->cfg.presets[index], reply + 3, cap - 3);
  }
  if (a->editing)
    return -1;
  Config updated = a->cfg;
  if (sscanf(request, "PRESET_PUT %d %n", &index, &used) == 1 && used) {
    RingPreset p;
    if (preset_parse(&p, request + used) || preset_put(&updated, index, &p))
      return -1;
  } else if (sscanf(request, "PRESET_DEL %d %c", &index, &extra) == 1) {
    if (preset_delete(&updated, index))
      return -1;
  } else
    return -1;
  if (config_validate(&updated) || config_save(&updated))
    return -1;
  a->cfg = updated;
  a->config_dirty = false;
  a->ring.last = 0;
  a->ring.selected = -1;
  a->ring.next = now;
  snprintf(reply, cap, "OK saved");
  return 0;
}
