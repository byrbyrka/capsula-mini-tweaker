#include "capsule.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char boot_mark[] = "# capsule-tweaker boot-sound: ";
static const char display_mark[] =
    "# capsule-controls replaced this display loop: ";
static bool boot_line(const char *p) {
  while (isspace((unsigned char)*p))
    p++;
  return (!strncmp(p, "mpg123 ", 7) || !strncmp(p, "/usr/bin/mpg123 ", 16)) &&
         strstr(p, "kde_startup_sound.mp3") && !strchr(p, ';') &&
         !strstr(p, "&&") && !strstr(p, "||");
}
static bool display_line(const char *p) {
  while (isspace((unsigned char)*p))
    p++;
  const char *boot = "clockdisp --anim blink && clockdisp --anim angry && "
                     "clockdisp --anim wink &";
  if (!strncmp(p, boot, strlen(boot)) &&
      (p[strlen(boot)] == '\n' || p[strlen(boot)] == 0))
    return true;
  return !strncmp(p, "nohup sh -c ", 12) &&
         strstr(p,
                "while true; do clockdisp --anim wink loop; sleep 1; done") !=
             NULL;
}
static void backup(const char *path, mode_t mode) {
  char dest[MAXPATH];
  snprintf(dest, sizeof dest, "%s.before-capsule-controls", path);
  int out = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 0777);
  if (out < 0)
    return;
  int in = open(path, O_RDONLY | O_CLOEXEC);
  if (in >= 0) {
    char b[4096];
    ssize_t n;
    while ((n = read(in, b, sizeof b)) > 0)
      if (write(out, b, (size_t)n) != n)
        break;
    close(in);
  }
  fsync(out);
  close(out);
}
// mode: 0 disable sound, 1 enable sound, 2 disable legacy display, 3 restore, 4
// inspect.
static int edit_file(const char *path, int mode, int *marked) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  struct stat st;
  if (fstat(fileno(f), &st)) {
    fclose(f);
    return -1;
  }
  char *line = NULL, *result = NULL;
  size_t linecap = 0, total = 0;
  int changes = 0;
  while (getline(&line, &linecap, f) >= 0) {
    const char *p = line, *prefix = "";
    bool is_marked = !strncmp(p, boot_mark, strlen(boot_mark));
    if (is_marked && marked)
      (*marked)++;
    if (mode == 4) {
      if (boot_line(p))
        changes++;
      continue;
    }
    if ((mode == 1 || mode == 3) && is_marked) {
      p += strlen(boot_mark);
      changes++;
    } else if (mode == 3 && !strncmp(p, display_mark, strlen(display_mark))) {
      p += strlen(display_mark);
      changes++;
    } else if (mode == 0 && boot_line(p)) {
      prefix = boot_mark;
      changes++;
    } else if (mode == 2 && display_line(p)) {
      prefix = display_mark;
      changes++;
    }
    size_t add = strlen(prefix) + strlen(p);
    if (total + add > 1024 * 1024) {
      changes = -1;
      break;
    }
    char *grow = realloc(result, total + add + 1);
    if (!grow) {
      changes = -1;
      break;
    }
    result = grow;
    memcpy(result + total, prefix, strlen(prefix));
    total += strlen(prefix);
    memcpy(result + total, p, strlen(p));
    total += strlen(p);
    result[total] = 0;
  }
  if (ferror(f))
    changes = -1;
  fclose(f);
  free(line);
  if (changes > 0 && mode != 4) {
    if (mode == 0 || mode == 2)
      backup(path, st.st_mode);
    if (atomic_text(path, result ? result : ""))
      changes = -1;
    else {
      chmod(path, st.st_mode & 07777);
      chown(path, st.st_uid, st.st_gid);
    }
  }
  free(result);
  return changes;
}
static int edit_startup(int mode, int *marked) {
  char pattern[MAXPATH], rc[MAXPATH];
  path_for(pattern, sizeof pattern, "/etc/local.d/*.start");
  glob_t g = {0};
  int total = 0;
  if (!glob(pattern, 0, NULL, &g))
    for (size_t i = 0; i < g.gl_pathc; i++) {
      int n = edit_file(g.gl_pathv[i], mode, marked);
      if (n < 0) {
        globfree(&g);
        return -1;
      }
      total += n;
    }
  globfree(&g);
  path_for(rc, sizeof rc, "/etc/rc.local");
  int n = edit_file(rc, mode, marked);
  return n < 0 ? -1 : total + n;
}
int startup_sound(int enabled) {
  int marked = 0;
  if (enabled < 0) {
    int active = edit_startup(4, &marked);
    return active > 0 ? 1 : marked > 0 ? 0 : -1;
  }
  return edit_startup(enabled ? 1 : 0, NULL);
}
int startup_prepare(bool first) {
  legacy_stop();
  if (edit_startup(2, NULL) < 0)
    return -1;
  if (first && startup_sound(0) < 0)
    return -1;
  return 0;
}
int startup_restore(void) { return edit_startup(3, NULL); }
static const char *base(const char *p) {
  const char *slash = strrchr(p, '/');
  return slash ? slash + 1 : p;
}
void legacy_stop(void) {
#ifdef TESTING
  if (*root_prefix)
    return;
#endif
  struct process {
    pid_t pid, parent;
    bool kill;
  } list[2048];
  size_t count = 0;
  DIR *dir = opendir("/proc");
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) && count < 2048) {
    if (!isdigit((unsigned char)entry->d_name[0]))
      continue;
    pid_t pid = (pid_t)atoi(entry->d_name);
    if (pid == getpid())
      continue;
    char path[100], cmd[4096], stat[2048];
    snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);
    int n = read_text(path, cmd, sizeof cmd);
    if (n <= 0)
      continue;
    char *args[16];
    int argc = 0;
    for (int off = 0; off < n && argc < 16;) {
      args[argc++] = cmd + off;
      off += (int)strlen(cmd + off) + 1;
    }
    bool shell = argc > 0 && (!strcmp(base(args[0]), "sh") ||
                              !strcmp(base(args[0]), "ash") ||
                              !strcmp(base(args[0]), "bash"));
    bool match =
        argc == 3 && shell && !strcmp(args[1], "-c") &&
        !strcmp(args[2],
                "while true; do clockdisp --anim wink loop; sleep 1; done");
    int o = shell ? 1 : 0;
    if (argc - o == 4 && !strcmp(base(args[o]), "clockdisp") &&
        !strcmp(args[o + 1], "--anim") && !strcmp(args[o + 2], "wink") &&
        !strcmp(args[o + 3], "loop"))
      match = true;
    snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    pid_t parent = 0;
    if (read_text(path, stat, sizeof stat) > 0) {
      char *p = strstr(stat, "PPid:");
      if (p)
        parent = (pid_t)atoi(p + 5);
    }
    list[count++] = (struct process){pid, parent, match};
  }
  closedir(dir);
  bool changed;
  do {
    changed = false;
    for (size_t i = 0; i < count; i++)
      if (!list[i].kill)
        for (size_t j = 0; j < count; j++)
          if (list[j].kill && list[i].parent == list[j].pid) {
            list[i].kill = true;
            changed = true;
            break;
          }
  } while (changed);
  for (size_t i = 0; i < count; i++)
    if (list[i].kill)
      kill(list[i].pid, SIGTERM);
}
