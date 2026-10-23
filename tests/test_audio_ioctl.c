#include "capsule.h"
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned checks, writes, closes;
static int wrong_range, reject_write;
static struct {
  unsigned id;
  long first, second;
} recorded[16];
#define CHECK(x)                                                               \
  do {                                                                         \
    assert(x);                                                                 \
    checks++;                                                                  \
  } while (0)
int __wrap_open(const char *path, int flags, ...) {
  CHECK(!strcmp(path, "/dev/snd/controlC0"));
  CHECK(flags & O_RDWR);
  return 42;
}
int __wrap_close(int fd) {
  CHECK(fd == 42);
  closes++;
  return 0;
}
int __wrap_ioctl(int fd, unsigned long command, ...) {
  CHECK(fd == 42);
  va_list ap;
  va_start(ap, command);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (command == SNDRV_CTL_IOCTL_ELEM_INFO) {
    struct snd_ctl_elem_info *info = arg;
    CHECK(info->id.iface == SNDRV_CTL_ELEM_IFACE_MIXER);
    if (!strcmp((char *)info->id.name, "Master Playback Volume")) {
      info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
      info->count = 2;
      info->value.integer.min = 0;
      info->value.integer.max = wrong_range ? 255 : 100;
      info->id.numid = 1;
    } else {
      CHECK(!strcmp((char *)info->id.name, "Playback Switch"));
      info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
      info->count = 1;
      info->id.numid = 2;
    }
    return 0;
  }
  struct snd_ctl_elem_value *value = arg;
  if (command == SNDRV_CTL_IOCTL_ELEM_READ) {
    value->value.integer.value[0] = value->id.numid == 1 ? 50 : 1;
    value->value.integer.value[1] = 50;
    return 0;
  }
  CHECK(command == SNDRV_CTL_IOCTL_ELEM_WRITE);
  if (reject_write)
    return -1;
  CHECK(writes < 16);
  recorded[writes].id = value->id.numid;
  recorded[writes].first = value->value.integer.value[0];
  recorded[writes].second = value->value.integer.value[1];
  writes++;
  return 0;
}
int main(void) {
  Audio audio;
  Config cfg;
  State state = {0};
  config_defaults(&cfg);
  CHECK(audio_open(&audio, &cfg, &state) == 0);
  CHECK(state.volume == 50 && !state.muted);
  CHECK(audio_apply(&audio, &(State){0, 0, 2}) == 0);
  CHECK(writes == 2);
  CHECK(recorded[0].id == 2 && recorded[0].first == 0);
  CHECK(recorded[1].id == 1 && recorded[1].first == 0 &&
        recorded[1].second == 0);
  writes = 0;
  CHECK(audio_apply(&audio, &(State){65, 1, 2}) == 0);
  CHECK(recorded[0].id == 2 && recorded[0].first == 0);
  CHECK(recorded[1].id == 1 && recorded[1].first == 65 &&
        recorded[1].second == 65);
  writes = 0;
  CHECK(audio_apply(&audio, &(State){65, 0, 2}) == 0);
  CHECK(recorded[0].id == 1 && recorded[0].first == 65);
  CHECK(recorded[1].id == 2 && recorded[1].first == 1);
  reject_write = 1;
  CHECK(audio_apply(&audio, &(State){70, 0, 2}) < 0);
  reject_write = 0;
  audio_close(&audio);
  CHECK(closes == 1);
  wrong_range = 1;
  CHECK(audio_open(&audio, &cfg, &state) < 0);
  CHECK(closes == 2);
  wrong_range = 0;
  memset(cfg.volume_control, 'A', sizeof cfg.volume_control - 1);
  cfg.volume_control[sizeof cfg.volume_control - 1] = 0;
  CHECK(audio_open(&audio, &cfg, &state) < 0);
  CHECK(closes == 3);
  printf("Native ALSA ioctl: %u assertions passed\n", checks);
  return 0;
}
