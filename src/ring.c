#include "capsule.h"
#include "ring_palette.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>

static void rotate(const Config *c, const unsigned char src[36],
                   unsigned char out[36]) {
  for (int i = 0; i < 12; i++) {
    int source = (i - c->ring_front + 18) % 12;
    memcpy(out + i * 3, src + source * 3, 3);
  }
}
void ring_frame(const Config *c, int volume, bool muted, int feedback,
                unsigned char out[36]) {
  unsigned char src[36] = {0};
  if (muted || feedback == FEEDBACK_FRONT)
    memcpy(src, mute_palette, 36);
  else if (feedback == FEEDBACK_VOLUME) {
    if (volume > 0)
      memcpy(src, volume_palette[volume], 36);
  } else if (feedback == FEEDBACK_ACTION)
    for (int i = 5; i <= 7; i++) {
      src[i * 3] = 0;
      src[i * 3 + 1] = 255;
      src[i * 3 + 2] = 219;
    }
  else if (c->effect >= FX_COUNT && c->effect < ring_effect_count(c)) {
    const RingPreset *p = &c->presets[c->effect - FX_COUNT];
    if (p->effect == FX_FRAMES || p->effect == FX_SOLID)
      memcpy(src, p->frames[0], 36);
  } else if (c->effect == FX_SOLID) {
    unsigned v = (unsigned)strtoul(c->ring_color, NULL, 16);
    for (int i = 0; i < 12; i++) {
      src[i * 3] = (v >> 16) & 255;
      src[i * 3 + 1] = (v >> 8) & 255;
      src[i * 3 + 2] = v & 255;
    }
  }
  rotate(c, src, out);
}
// AirPlay state: 1 = sender connected (steady dim ring), 2 = playing
// (a dot running around the ring). The tweaker stays the only ring writer.
static void ap2_frame(int state, double now, unsigned char out[36]) {
  memset(out, 0, 36);
  if (state == 1) {
    for (int i = 0; i < 12; i++) {
      out[i * 3] = 0;
      out[i * 3 + 1] = 24;
      out[i * 3 + 2] = 72;
    }
    return;
  }
  int pos = ((int)(now * 2)) % 12;
  if (pos < 0)
    pos += 12;
  for (int i = 0; i < 12; i++) {
    if (i == pos) {
      out[i * 3] = 0;
      out[i * 3 + 1] = 255;
      out[i * 3 + 2] = 219;
    } else {
      out[i * 3] = 0;
      out[i * 3 + 1] = 16;
      out[i * 3 + 2] = 14;
    }
  }
}

static uint64_t frame_hash(const unsigned char *b, int brightness) {
  uint64_t h = (uint64_t)brightness + 1;
  for (int i = 0; i < 36; i++)
    h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}
void ring_tick(App *a, double now) {
  Ring *r = &a->ring;
  r->next = 0;
  if (now < r->retry)
    return;
  if (r->selected != a->cfg.effect) {
    r->selected = a->cfg.effect;
    r->epoch = now;
  }
  bool muted = a->state.muted && !a->editing;
  int feedback = now < a->feedback_until ? a->feedback : FEEDBACK_NONE;
  if (a->editing)
    feedback = FEEDBACK_NONE;
  bool ap2 = a->ap2_state != 0 && !muted && feedback == FEEDBACK_NONE;
  const RingPreset *p =
      a->cfg.effect >= FX_COUNT && a->cfg.effect < ring_effect_count(&a->cfg)
          ? &a->cfg.presets[a->cfg.effect - FX_COUNT]
          : NULL;
  int effect = p ? p->effect : a->cfg.effect;
  int brightness = p && !muted && feedback == FEEDBACK_NONE
                       ? p->brightness
                       : a->cfg.ring_brightness;
  int speed_ms = p ? p->speed : a->cfg.ring_speed;
  bool animated = !muted && feedback == FEEDBACK_NONE && !ap2 &&
                  (effect == FX_RAINBOW || effect == FX_FIRE);
  char bright[12];
  snprintf(bright, sizeof bright, "%d", brightness);
  if (animated) {
    int settings = effect + 10 * brightness + 2000 * speed_ms;
    if (r->child > 0) {
      int status;
      pid_t done = waitpid(r->child, &status, WNOHANG);
      if (done == 0 && r->active_effect == settings)
        return;
      if (done > 0 && (!WIFEXITED(status) || WEXITSTATUS(status))) {
        stop_child(&r->child);
        r->retry = now + 5;
        r->last = 0;
        return;
      }
      stop_child(&r->child);
    }
    char speed[20];
    snprintf(speed, sizeof speed, "%d", speed_ms);
    char *rainbow[] = {a->cfg.ringled, "-b", bright,    "--duration", "60000",
                       "--fps",        "8",  "rainbow", speed,        NULL};
    char *fire[] = {a->cfg.ringled, "-b", bright, "--duration", "60000",
                    "--fps",        "8",  "fire", NULL};
    r->child = spawn_cmd(effect == FX_RAINBOW ? rainbow : fire, -1);
    r->active_effect = settings;
    r->last = 0;
    if (r->child < 0)
      r->retry = now + 5;
    return;
  }
  stop_child(&r->child);
  unsigned char frame[36];
  if (ap2) {
    unsigned char src[36];
    ap2_frame(a->ap2_state, now, src);
    rotate(&a->cfg, src, frame);
    if (a->ap2_state == 2)
      r->next = now + 0.2;
  } else {
    ring_frame(&a->cfg, a->state.volume, muted, feedback, frame);
    if (p && !muted && feedback == FEEDBACK_NONE && effect == FX_FRAMES) {
      unsigned char src[36];
      preset_frame(p, now - r->epoch, src);
      rotate(&a->cfg, src, frame);
      if (p->count > 1) {
        unsigned long long index =
            (unsigned long long)((now - r->epoch) * 1000 / p->frame_ms);
        r->next = r->epoch + (index + 1) * (p->frame_ms / 1000.0);
      }
    }
  }
  uint64_t hash = frame_hash(frame, brightness);
  if (hash == r->last)
    return;
  char colors[12][7];
  char *args[17] = {a->cfg.ringled, "-b", bright, "raw"};
  for (int i = 0; i < 12; i++) {
    snprintf(colors[i], 7, "%02x%02x%02x", frame[i * 3], frame[i * 3 + 1],
             frame[i * 3 + 2]);
    args[i + 4] = colors[i];
  }
  args[16] = NULL;
  if (!run_cmd(args, 1))
    r->last = hash;
  else {
    r->retry = now + 5;
    logmsg(LOG_WARNING, "ringled failed; retry in 5 seconds");
  }
}
void ring_close(App *a) {
  stop_child(&a->ring.child);
  char *args[] = {a->cfg.ringled, "off", NULL};
  run_cmd(args, 1);
}
