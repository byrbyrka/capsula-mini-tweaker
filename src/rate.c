#include "capsule.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static int decimal_micro(const char *s, uint64_t *out) {
  if (!isdigit((unsigned char)*s))
    return -1;
  uint64_t whole = 0, fraction = 0, scale = 100000;
  while (isdigit((unsigned char)*s)) {
    whole = whole * 10 + (unsigned)(*s++ - '0');
    if (whole > 1000000)
      return -1;
  }
  if (*s == '.' || *s == ',') {
    s++;
    while (isdigit((unsigned char)*s)) {
      if (scale) {
        fraction += (unsigned)(*s - '0') * scale;
        scale /= 10;
      }
      s++;
    }
  }
  if (*s != '<' && *s != '\0')
    return -1;
  *out = whole * 1000000 + fraction;
  return 0;
}
int rate_parse(const char *xml, uint64_t *micro, char date[16]) {
  const char *usd = strstr(xml, "<CharCode>USD</CharCode>");
  if (!usd)
    return -1;
  const char *start = xml, *scan = xml, *end = strstr(usd, "</Valute>");
  if (!end)
    return -1;
  while ((scan = strstr(scan, "<Valute ")) && scan < usd) {
    start = scan;
    scan++;
  }
  // Minimal fixtures may use <Valute> without an ID attribute.
  scan = xml;
  while ((scan = strstr(scan, "<Valute>")) && scan < usd) {
    if (scan > start)
      start = scan;
    scan++;
  }
  const char *value = strstr(start, "<Value>"),
             *nominal = strstr(start, "<Nominal>");
  if (!value || !nominal || value > end || nominal > end)
    return -1;
  uint64_t raw;
  if (decimal_micro(value + 7, &raw))
    return -1;
  char *tail;
  long n = strtol(nominal + 9, &tail, 10);
  if (n <= 0 || n > 1000000 || strncmp(tail, "</Nominal>", 10))
    return -1;
  uint64_t result = (raw + (unsigned)n / 2) / (unsigned)n;
  if (!result || result >= 10000000000ULL)
    return -1;
  const char *d = strstr(xml, "Date=\"");
  int day, month, year;
  if (!d || sscanf(d + 6, "%2d.%2d.%4d", &day, &month, &year) != 3 || day < 1 ||
      day > 31 || month < 1 || month > 12 || year < 2000 || year > 2199)
    return -1;
  snprintf(date, 16, "%02d.%02d.%04d", day, month, year);
  *micro = result;
  return 0;
}
void rate_startup(Rate *r) {
  memset(r, 0, sizeof *r);
  r->fd = -1;
  char p[MAXPATH], data[256];
  path_for(p, sizeof p, RATE_PATH);
  unsigned long long micro = 0;
  long long fetched = 0;
  if (read_text(p, data, sizeof data) > 0 &&
      sscanf(data, "%llu %15s %lld", &micro, r->date, &fetched) == 3 &&
      micro > 0 && micro < 10000000000ULL) {
    r->micro = micro;
    r->fetched = (time_t)fetched;
  } else {
    path_for(p, sizeof p, "/var/lib/capsule-controls/usd.json");
    if (read_text(p, data, sizeof data) > 0) {
      char value[40];
      if (!json_string(data, "value", value, sizeof value))
        decimal_micro(value, &r->micro);
      json_string(data, "date", r->date, sizeof r->date);
    }
  }
  time_t age = time(NULL) - r->fetched;
  r->due =
      monotime() + ((r->micro && age >= 0 && age < 21600) ? 21600 - age : 0);
}
static void finish(Rate *r, bool success) {
  if (r->fd >= 0) {
    close(r->fd);
    r->fd = -1;
  }
  int status = 0;
  pid_t p = r->child;
  if (p > 0) {
    pid_t done = waitpid(p, &status, WNOHANG);
    if (done == 0) {
      stop_child(&r->child);
    } else
      r->child = 0;
  }
  uint64_t micro;
  char date[16];
  r->xml[r->used] = 0;
  if (success && !rate_parse(r->xml, &micro, date)) {
    r->micro = micro;
    snprintf(r->date, sizeof r->date, "%s", date);
    r->fetched = time(NULL);
    r->due = monotime() + 21600;
    char path[MAXPATH], data[160];
    path_for(path, sizeof path, RATE_PATH);
    snprintf(data, sizeof data, "%llu %s %lld\n", (unsigned long long)micro,
             date, (long long)r->fetched);
    atomic_text(path, data);
  } else {
    r->due = monotime() + 900;
    logmsg(LOG_WARNING, "CBR update failed; keeping cached USD rate");
  }
}
void rate_tick(Rate *r, double now) {
  if (r->child > 0) {
    if (now >= r->deadline)
      finish(r, false);
    return;
  }
  if (now < r->due)
    return;
#ifdef TESTING
  if (*root_prefix) {
    r->due = now + 21600;
    return;
  }
#endif
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC)) {
    r->due = now + 900;
    return;
  }
  char *argv[] = {"wget",
                  "-q",
                  "-T",
                  "8",
                  "-O",
                  "-",
                  "https://www.cbr.ru/scripts/XML_daily.asp",
                  NULL};
  r->child = spawn_cmd(argv, pipefd[1]);
  close(pipefd[1]);
  r->fd = pipefd[0];
  fcntl(r->fd, F_SETFL, O_NONBLOCK);
  r->used = 0;
  r->deadline = now + 12;
  if (r->child < 0) {
    close(r->fd);
    r->fd = -1;
    r->child = 0;
    r->due = now + 900;
  }
}
void rate_read(Rate *r) {
  if (r->fd < 0)
    return;
  for (;;) {
    if (r->used >= sizeof r->xml - 1) {
      finish(r, false);
      return;
    }
    ssize_t n = read(r->fd, r->xml + r->used, sizeof r->xml - 1 - r->used);
    if (n > 0) {
      r->used += (size_t)n;
      continue;
    }
    if (n == 0) {
      finish(r, true);
      return;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      finish(r, false);
    return;
  }
}
void rate_close(Rate *r) {
  stop_child(&r->child);
  if (r->fd >= 0)
    close(r->fd);
  r->fd = -1;
}
