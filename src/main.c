#include "capsule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char **argv) {
  openlog("capsule-tweaker", LOG_PID, LOG_DAEMON);
#ifdef TESTING
  const char *root = getenv("CAPSULE_TEST_ROOT");
  if (root)
    root_prefix = root;
  if (argc == 2 && !strcmp(argv[1], "--test-preset-menu")) {
    presets_menu();
    return 0;
  }
#endif
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--config") && i + 1 < argc)
      config_override = argv[++i];
  if (argc == 1)
    return menu_run();
  if (!strcmp(argv[1], "--version")) {
    puts("capsule-tweaker " VERSION);
    return 0;
  }
  if (!strcmp(argv[1], "--daemon"))
    return service_run();
  if (!strcmp(argv[1], "--preflight")) {
    Config c;
    State s = {0};
    Audio audio;
    Display display;
    if (config_load(&c) || audio_open(&audio, &c, &s))
      return 1;
    audio_close(&audio);
    c.grab = 0;
    int fd = input_open(&c);
    if (fd < 0)
      return 1;
    close(fd);
    if (display_open(&display) || access(c.ringled, X_OK))
      return 1;
    return 0;
  }
  if (!strcmp(argv[1], "--prepare")) {
    Config c;
    if (config_load(&c) || config_save(&c))
      return 1;
    return startup_prepare(argc > 2 && !strcmp(argv[2], "first")) < 0 ? 1 : 0;
  }
  if (!strcmp(argv[1], "--fetch-rate")) {
    char xml[32768];
    size_t used = fread(xml, 1, sizeof xml - 1, stdin);
    xml[used] = 0;
    uint64_t micro;
    char date[16];
    if (rate_parse(xml, &micro, date))
      return 1;
    printf("%llu %s\n", (unsigned long long)micro, date);
    return 0;
  }
  fputs("Для открытия меню запускайте capsule-tweaker без аргументов.\n",
        stderr);
  return 1;
}
