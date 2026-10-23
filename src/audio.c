#include "capsule.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef TESTING
static int fake_read(State *s) {
  char p[MAXPATH], b[128];
  path_for(p, sizeof p, "/hardware");
  if (read_text(p, b, sizeof b) < 0)
    return -1;
  return sscanf(b, "%d %d", &s->volume, &s->muted) == 2 ? 0 : -1;
}
static int fake_write(const State *s) {
  char p[MAXPATH], b[128];
  path_for(p, sizeof p, "/hardware");
  snprintf(b, sizeof b, "%d %d\n", s->volume, s->muted || s->volume == 0);
  return atomic_text(p, b);
}
#endif
/* Kernel ALSA control ABI: no libasound, amixer or helper process is needed. */
static int element(Audio *a, const char *name, struct snd_ctl_elem_value *value,
                   unsigned *count, bool volume) {
  struct snd_ctl_elem_info info = {0};
  if (strlen(name) >= sizeof info.id.name)
    return -1;
  info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
  memcpy(info.id.name, name, strlen(name) + 1);
  if (ioctl(a->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || info.count < 1 ||
      info.count > 128)
    return -1;
  if (volume && (info.type != SNDRV_CTL_ELEM_TYPE_INTEGER ||
                 info.value.integer.min != 0 || info.value.integer.max != 100))
    return -1;
  if (!volume && info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN)
    return -1;
  *count = info.count;
  value->id = info.id;
  return ioctl(a->fd, SNDRV_CTL_IOCTL_ELEM_READ, value) < 0 ? -1 : 0;
}
int audio_open(Audio *a, const Config *c, State *s) {
  memset(a, 0, sizeof *a);
  a->fd = -1;
#ifdef TESTING
  if (*root_prefix)
    return fake_read(s);
#endif
  int card;
  if (parse_int(c->card, &card) || card < 0 || card > 31)
    return -1;
  char device[80];
  snprintf(device, sizeof device, "/dev/snd/controlC%d", card);
  a->fd = open(device, O_RDWR | O_CLOEXEC);
  if (a->fd < 0)
    return -1;
  if (element(a, c->volume_control, &a->volume, &a->volume_count, true) ||
      element(a, c->mute_control, &a->mute, &a->mute_count, false)) {
    audio_close(a);
    return -1;
  }
  s->volume = volume_step((int)a->volume.value.integer.value[0]);
  s->muted = !a->mute.value.integer.value[0];
  return 0;
}
int audio_apply(Audio *a, const State *s) {
#ifdef TESTING
  if (*root_prefix)
    return fake_write(s);
#endif
  bool silent = s->muted || s->volume == 0;
  if (silent) {
    for (unsigned i = 0; i < a->mute_count; i++)
      a->mute.value.integer.value[i] = 0;
    if (ioctl(a->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &a->mute) < 0)
      return -1;
  }
  for (unsigned i = 0; i < a->volume_count; i++)
    a->volume.value.integer.value[i] = s->volume;
  if (ioctl(a->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &a->volume) < 0)
    return -1;
  if (!silent) {
    for (unsigned i = 0; i < a->mute_count; i++)
      a->mute.value.integer.value[i] = 1;
    if (ioctl(a->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &a->mute) < 0)
      return -1;
  }
  return 0;
}
void audio_close(Audio *a) {
  if (a->fd >= 0)
    close(a->fd);
  memset(a, 0, sizeof *a);
  a->fd = -1;
}
