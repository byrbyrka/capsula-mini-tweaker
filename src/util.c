#include "capsule.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

const char *root_prefix = "";
const char *config_override = NULL;
double monotime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
void path_for(char *out, size_t cap, const char *p) {
  snprintf(out, cap, "%s%s", root_prefix, p);
}
int read_text(const char *p, char *out, size_t cap) {
  if (cap < 2)
    return -1;
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, out, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  out[n] = 0;
  return (int)n;
}
int write_bytes(const char *p, const void *data, size_t n) {
  int fd = open(p, O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t done = write(fd, data, n);
  int err = done == (ssize_t)n ? 0 : -1;
  close(fd);
  return err;
}
int mkdirs(const char *path) {
  char p[MAXPATH];
  snprintf(p, sizeof p, "%s", path);
  for (char *q = p + 1; *q; q++)
    if (*q == '/') {
      *q = 0;
      if (mkdir(p, 0755) && errno != EEXIST)
        return -1;
      *q = '/';
    }
  return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}
int atomic_text(const char *path, const char *text) {
  char tmp[MAXPATH], parent[MAXPATH];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  snprintf(parent, sizeof parent, "%s", path);
  char *slash = strrchr(parent, '/');
  if (slash) {
    *slash = 0;
    if (mkdirs(parent))
      return -1;
  }
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  size_t len = strlen(text);
  bool ok = write(fd, text, len) == (ssize_t)len && fsync(fd) == 0;
  close(fd);
  if (!ok || rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  return 0;
}
pid_t spawn_cmd(char *const argv[], int output) {
  pid_t p = fork();
  if (p != 0)
    return p;
  setpgid(0, 0);
  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, 0);
    dup2(devnull, 2);
    dup2(output >= 0 ? output : devnull, 1);
    if (devnull > 2)
      close(devnull);
  }
  execvp(argv[0], argv);
  _exit(127);
}
void stop_child(pid_t *pid) {
  if (*pid <= 0)
    return;
  kill(-*pid, SIGTERM);
  double until = monotime() + .3;
  int status;
  while (waitpid(*pid, &status, WNOHANG) == 0 && monotime() < until)
    usleep(10000);
  kill(-*pid, SIGKILL);
  while (waitpid(*pid, &status, 0) < 0 && errno == EINTR) {
  }
  *pid = 0;
}
int run_cmd(char *const argv[], double timeout) {
  pid_t p = spawn_cmd(argv, -1);
  if (p < 0)
    return -1;
  int status;
  double until = monotime() + timeout;
  for (;;) {
    pid_t r = waitpid(p, &status, WNOHANG);
    if (r == p)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    if (r < 0 && errno != EINTR)
      return -1;
    if (monotime() >= until) {
      stop_child(&p);
      return -1;
    }
    usleep(5000);
  }
}
void logmsg(int priority, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsyslog(priority, fmt, ap);
  va_end(ap);
}
int parse_int(const char *s, int *out) {
  if (!s || !*s)
    return -1;
  char *end;
  errno = 0;
  long n = strtol(s, &end, 10);
  if (end == s)
    return -1;
  while (isspace((unsigned char)*end))
    end++;
  if (errno || *end || n < -1000000 || n > 1000000)
    return -1;
  *out = (int)n;
  return 0;
}
static const char *json_value(const char *text, const char *key) {
  char pattern[100];
  snprintf(pattern, sizeof pattern, "\"%s\"", key);
  const char *p = strstr(text, pattern);
  if (!p)
    return NULL;
  p += strlen(pattern);
  while (isspace((unsigned char)*p))
    p++;
  if (*p++ != ':')
    return NULL;
  while (isspace((unsigned char)*p))
    p++;
  return p;
}
int json_int(const char *text, const char *key, int fallback) {
  const char *p = json_value(text, key);
  if (!p)
    return fallback;
  if (!strncmp(p, "true", 4))
    return 1;
  if (!strncmp(p, "false", 5))
    return 0;
  char *end;
  long n = strtol(p, &end, 10);
  if (end == p || n < -1000000 || n > 1000000)
    return fallback;
  return (int)n;
}
int json_string(const char *text, const char *key, char *out, size_t cap) {
  const char *p = json_value(text, key);
  if (!p || *p++ != '"')
    return -1;
  const char *end = strchr(p, '"');
  if (!end || (size_t)(end - p) >= cap || memchr(p, '\\', end - p))
    return -1;
  memcpy(out, p, end - p);
  out[end - p] = 0;
  return 0;
}
