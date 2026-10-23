#include "capsule.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int address(struct sockaddr_un *a, const char *path) {
  memset(a, 0, sizeof *a);
  a->sun_family = AF_UNIX;
  if (strlen(path) >= sizeof a->sun_path)
    return -1;
  strcpy(a->sun_path, path);
  return 0;
}
int ipc_open(void) {
  char path[MAXPATH];
  path_for(path, sizeof path, SOCKET_PATH);
  struct sockaddr_un addr;
  if (address(&addr, path))
    return -1;
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return -1;
  unlink(path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || chmod(path, 0600)) {
    close(fd);
    return -1;
  }
  return fd;
}
static void status(App *a, char *out, size_t cap) {
  snprintf(out, cap, "OK %d %d %d %d %d %d %d %d %d %d %d %d %d %llu %s",
           a->state.volume, a->state.muted, a->state.mode, a->cfg.effect,
           a->cfg.face, a->editing, a->cfg.brightness, a->cfg.current_divider,
           a->cfg.frame_ms, a->cfg.ring_brightness, a->cfg.ring_ms,
           a->cfg.ring_front, a->cfg.ring_speed,
           (unsigned long long)a->rate.micro,
           *a->rate.date ? a->rate.date : "-");
}
void ipc_receive(App *a, double now) {
  char request[PRESET_TEXT + 80], reply[PRESET_TEXT + 80];
  struct sockaddr_un peer;
  socklen_t length = sizeof peer;
  ssize_t n = recvfrom(a->socket_fd, request, sizeof request - 1, MSG_TRUNC,
                       (struct sockaddr *)&peer, &length);
  if (n < 0)
    return;
  bool oversized = (size_t)n >= sizeof request - 1;
  request[oversized ? 0 : (size_t)n] = 0;
  int ok = 0, value;
  bool own_reply = false;
  char key[80], arg[160], extra;
  if (oversized)
    ok = -1;
  else if (!strcmp(request, "GET")) {
  } else if (!strcmp(request, "MUTE"))
    ok = app_update(a, a->state.volume, !a->state.muted, a->state.mode, now);
  else if (sscanf(request, "VOL %d %c", &value, &extra) == 1)
    ok = app_update(a, value, 0, a->state.mode, now);
  else if (sscanf(request, "MODE %d %c", &value, &extra) == 1)
    ok = app_update(a, a->state.volume, a->state.muted, value, now);
  else if (sscanf(request, "RING %d %c", &value, &extra) == 1) {
    if (value < 0 || value >= ring_effect_count(&a->cfg))
      ok = -1;
    else
      app_change_effect(a, value, now);
  } else if (!strcmp(request, "PREVIEW")) {
    a->feedback = FEEDBACK_FRONT;
    a->feedback_until = now + 2;
  } else if (!strcmp(request, "EDIT"))
    app_edit(a, now);
  else if (!strncmp(request, "PRESET", 6)) {
    ok = preset_ipc(a, request, reply, sizeof reply, now);
    own_reply = true;
  } else if (sscanf(request, "SET %79s %159[^\n]", key, arg) == 2) {
    Config updated = a->cfg;
    ok = !strncmp(key, "preset", 6) ? -1 : config_set(&updated, key, arg);
    if (!ok)
      ok = config_validate(&updated);
    if (!ok) {
      if (!strcmp(key, "effect"))
        app_change_effect(a, updated.effect, now);
      else {
        a->cfg = updated;
        a->config_dirty = true;
        a->save_at = now + 2;
        a->ring.last = 0;
      }
    }
  } else
    ok = -1;
  if (ok)
    snprintf(reply, sizeof reply, "ERROR command rejected");
  else if (!own_reply)
    status(a, reply, sizeof reply);
  if (length > sizeof(sa_family_t))
    sendto(a->socket_fd, reply, strlen(reply), MSG_NOSIGNAL,
           (struct sockaddr *)&peer, length);
}
int ipc_call(const char *command, char *reply, size_t cap) {
  char name[80], client[MAXPATH], server[MAXPATH];
  snprintf(name, sizeof name, "/run/capsule-client-%ld", (long)getpid());
  path_for(client, sizeof client, name);
  path_for(server, sizeof server, SOCKET_PATH);
  struct sockaddr_un a, b;
  if (address(&a, client) || address(&b, server))
    return -1;
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  unlink(client);
  int result = -1;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || chmod(client, 0600) ||
      connect(fd, (struct sockaddr *)&b, sizeof b))
    goto done;
  if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0)
    goto done;
  struct pollfd p = {fd, POLLIN, 0};
  if (poll(&p, 1, 3000) <= 0)
    goto done;
  ssize_t n = recv(fd, reply, cap - 1, 0);
  if (n < 0)
    goto done;
  reply[n] = 0;
  result = strncmp(reply, "OK ", 3) ? -1 : 0;
done:
  close(fd);
  unlink(client);
  return result;
}
