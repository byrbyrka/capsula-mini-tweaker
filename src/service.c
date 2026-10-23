#include "capsule.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;
static void stop_signal(int sig) {
  (void)sig;
  stopping = 1;
}
struct key_event_wire {
  struct timeval timestamp;
  uint16_t type, code;
  int32_t value;
};
static void read_keys(App *a, double now) {
  ssize_t n = read(a->input_fd, a->input_bytes + a->input_used,
                   sizeof a->input_bytes - a->input_used);
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      return;
    close(a->input_fd);
    a->input_fd = -1;
    a->reconnect = now + 2;
    memset(&a->keys, 0, sizeof a->keys);
    a->input_used = 0;
    return;
  }
  a->input_used += (size_t)n;
  size_t used = 0;
  while (a->input_used - used >= sizeof(struct key_event_wire)) {
    struct key_event_wire e;
    memcpy(&e, a->input_bytes + used, sizeof e);
    used += sizeof e;
    if (e.type == 0 && e.code == 3)
      keys_drop(a);
    else if (e.type == 0 && e.code == 0 && a->keys.dropped) {
      unsigned char bits[KEY_COUNT / 8] = {0};
      a->keys.dropped = false;
      a->keys.blocked = false;
      if (ioctl(a->input_fd, _IOC(_IOC_READ, 'E', 0x18, sizeof bits), bits) < 0)
        a->keys.blocked = true;
      else
        for (int i = 0; i < KEY_COUNT; i++)
          if (bits[i / 8] & (1 << (i % 8))) {
            a->keys.down[i] = true;
            if (i == a->cfg.mute_key)
              a->keys.blocked = true;
            for (int j = 0; j < a->cfg.action_count; j++)
              if (i == a->cfg.action_keys[j])
                a->keys.blocked = true;
          }
    } else if (e.type == 1 && !a->keys.dropped)
      key_event(a, e.code, e.value, now);
  }
  memmove(a->input_bytes, a->input_bytes + used, a->input_used - used);
  a->input_used -= used;
}
static void save(App *a) {
  if (a->dirty && !state_save(&a->state))
    a->dirty = false;
  // A preview is temporary; persist only the last confirmed ring effect.
  if (a->config_dirty) {
    Config c = a->cfg;
    if (a->editing)
      c.effect = a->old_effect;
    if (!config_save(&c))
      a->config_dirty = false;
  }
}
int service_run(void) {
  static App app;
  App *a = &app;
  memset(a, 0, sizeof *a);
  a->input_fd = a->socket_fd = a->lock_fd = -1;
  if (config_load(&a->cfg)) {
    logmsg(LOG_ERR, "Invalid configuration");
    return 1;
  }
  setenv("TZ", a->cfg.timezone, 1);
  tzset();
  char path[MAXPATH];
  path_for(path, sizeof path, "/run");
  mkdirs(path);
  path_for(path, sizeof path, "/run/capsule-controls.lock");
  a->lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (a->lock_fd < 0 || flock(a->lock_fd, LOCK_EX | LOCK_NB)) {
    logmsg(LOG_ERR, "Another instance is running");
    if (a->lock_fd >= 0)
      close(a->lock_fd);
    return 1;
  }
  a->state.mode = a->cfg.initial_mode;
  if (audio_open(&a->audio, &a->cfg, &a->state)) {
    logmsg(LOG_ERR, "Cannot open ALSA controls");
    close(a->lock_fd);
    return 1;
  }
  state_load(&a->state);
  if (audio_apply(&a->audio, &a->state)) {
    audio_close(&a->audio);
    close(a->lock_fd);
    return 1;
  }
  a->socket_fd = ipc_open();
  if (a->socket_fd < 0) {
    audio_close(&a->audio);
    close(a->lock_fd);
    return 1;
  }
  display_open(&a->display);
  rate_startup(&a->rate);
  struct sigaction sa = {0};
  sa.sa_handler = stop_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  stopping = 0;
  while (!stopping) {
    double now = monotime();
    if (a->input_fd < 0 && now >= a->reconnect) {
      a->input_fd = input_open(&a->cfg);
      a->reconnect = now + 5;
    }
    if ((a->dirty || a->config_dirty) && now >= a->save_at) {
      save(a);
      a->save_at = now + 30;
    }
    rate_tick(&a->rate, now);
    ring_tick(a, now);
    display_tick(a, now);
    double next = now + 10;
#define DEADLINE(x)                                                            \
  do {                                                                         \
    double d = (x);                                                            \
    if (d > now && d < next)                                                   \
      next = d;                                                                \
  } while (0)
    DEADLINE(a->display.next);
    DEADLINE(a->feedback_until);
    DEADLINE(a->overlay_until);
    if (a->dirty || a->config_dirty)
      DEADLINE(a->save_at);
    if (a->input_fd < 0)
      DEADLINE(a->reconnect);
    if (a->rate.child > 0)
      DEADLINE(a->rate.deadline);
    else
      DEADLINE(a->rate.due);
    if (a->ring.child > 0 && next > now + 1)
      next = now + 1;
    if (!a->display.found)
      DEADLINE(a->display.retry);
    DEADLINE(a->ring.retry);
    DEADLINE(a->ring.next);
    struct pollfd p[3] = {{a->socket_fd, POLLIN, 0},
                          {a->input_fd, POLLIN, 0},
                          {a->rate.fd, POLLIN, 0}};
    int timeout = (int)((next - monotime()) * 1000);
    if (timeout < 1)
      timeout = 1;
    int result = poll(p, 3, timeout);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    now = monotime();
    if (p[0].revents & POLLIN)
      ipc_receive(a, now);
    if (p[1].revents & (POLLIN | POLLHUP | POLLERR))
      read_keys(a, now);
    if (p[2].revents & (POLLIN | POLLHUP | POLLERR))
      rate_read(&a->rate);
  }
  save(a);
  rate_close(&a->rate);
  ring_close(a);
  display_close(&a->display);
  audio_close(&a->audio);
  if (a->input_fd >= 0)
    close(a->input_fd);
  close(a->socket_fd);
  path_for(path, sizeof path, SOCKET_PATH);
  unlink(path);
  close(a->lock_fd);
  return 0;
}
