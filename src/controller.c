#include "capsule.h"
#include <stdio.h>
#include <syslog.h>

int app_update(App *a, int volume, int muted, int mode, double now) {
  State proposed = {volume, muted, mode};
  if (volume < 0 || volume > 100 || volume % 5 || muted < 0 || muted > 1 ||
      mode < 0 || mode > 2)
    return -1;
  if ((volume != a->state.volume || muted != a->state.muted) &&
      audio_apply(&a->audio, &proposed)) {
    audio_apply(&a->audio, &a->state);
    snprintf(a->overlay, sizeof a->overlay, "Err");
    a->overlay_until = now + 2;
    return -1;
  }
  bool changed = volume != a->state.volume || muted != a->state.muted ||
                 mode != a->state.mode;
  if (mode != a->state.mode) {
    a->overlay_until = 0;
    a->feedback = FEEDBACK_ACTION;
    a->feedback_until = now + .3;
  } else if (muted) {
    a->overlay_until = 0;
    a->feedback_until = 0;
  } else {
    snprintf(a->overlay, sizeof a->overlay, "%d", volume);
    a->overlay_until = now + 1.2;
    a->feedback = FEEDBACK_VOLUME;
    a->feedback_until = now + a->cfg.ring_ms / 1000.0;
  }
  a->state = proposed;
  if (changed) {
    a->dirty = true;
    a->save_at = now + 2;
    logmsg(LOG_INFO, "volume=%d muted=%d mode=%d", volume, muted, mode);
  }
  return 0;
}
void app_edit(App *a, double now) {
  if (a->editing)
    return;
  a->editing = true;
  a->old_effect = a->cfg.effect;
  a->feedback_until = now;
  a->overlay_until = 0;
  a->ring.last = 0;
}
void app_change_effect(App *a, int effect, double now) {
  if (effect < 0 || effect >= ring_effect_count(&a->cfg))
    return;
  a->cfg.effect = effect;
  a->ring.last = 0;
  a->ring.selected = -1;
  a->feedback_until = now;
  if (!a->editing) {
    a->config_dirty = true;
    a->save_at = now + 2;
  }
}
