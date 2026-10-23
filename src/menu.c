#include "capsule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int answer(const char *prompt, int low, int high, int step) {
  char line[100];
  printf("%s (Enter — отмена): ", prompt);
  fflush(stdout);
  if (!fgets(line, sizeof line, stdin) || line[0] == '\n')
    return -999;
  int value;
  if (parse_int(line, &value) || value < low || value > high || value % step) {
    puts("Недопустимое значение.");
    return -999;
  }
  return value;
}
static void call(const char *text) {
  char reply[512];
  if (ipc_call(text, reply, sizeof reply))
    puts("Не удалось выполнить команду. Проверьте, включена ли служба (пункт "
         "6).");
}
static void set(const char *key, int low, int high, int step) {
  int v = answer(key, low, high, step);
  if (v != -999) {
    char cmd[120];
    snprintf(cmd, sizeof cmd, "SET %s %d", key, v);
    call(cmd);
  }
}
static void screen_menu(void) {
  puts(
      "\n1 — яркость 0–255\n2 — делитель тока 1–4\n3 — длительность кадра, "
      "мс\n4 — выбрать мордашку\n5 — кнопка действия / центр / обе\n0 — назад");
  int x = answer(">", 0, 5, 1);
  switch (x) {
  case 1:
    set("brightness", 0, 255, 1);
    break;
  case 2:
    set("current_divider", 1, 4, 1);
    break;
  case 3:
    set("frame_ms", 100, 5000, 1);
    break;
  case 4:
    for (int i = 0; i < 6; i++)
      printf("%d — %s\n", i, face_names[i]);
    set("face", 0, 5, 1);
    break;
  case 5: {
    int n = answer("1 — действие; 2 — центр; 3 — обе", 1, 3, 1);
    if (n != -999)
      call(n == 1   ? "SET actions 582"
           : n == 2 ? "SET actions 148"
                    : "SET actions 582,148");
    break;
  }
  default:
    break;
  }
}
static void ring_menu(void) {
  puts("\n1 — выбрать эффект\n2 — яркость 0–100\n3 — время показа громкости, "
       "мс\n4 — скорость радуги: время оборота, мс\n5 — проверить передние "
       "красные светодиоды\n6 — номер переднего центрального светодиода\n7 — "
       "постоянный цвет RRGGBB\n8 — свои пресеты: создать / изменить / удалить\n0 — назад");
  int x = answer(">", 0, 8, 1);
  switch (x) {
  case 1: {
    int total = presets_choices();
    if (total > 0) set("effect", 0, total - 1, 1);
    break;
  }
  case 2:
    set("ring_brightness", 0, 100, 1);
    break;
  case 3:
    set("ring_ms", 100, 5000, 1);
    break;
  case 4:
    set("ring_speed", 500, 30000, 1);
    break;
  case 5:
    call("PREVIEW");
    break;
  case 6:
    puts("Штатный центр — 6: передние светодиоды 5, 6, 7.");
    set("ring_front", 0, 11, 1);
    break;
  case 7: {
    char color[32], cmd[80];
    printf("Цвет RRGGBB: ");
    if (fgets(color, sizeof color, stdin)) {
      color[strcspn(color, "\r\n")] = 0;
      snprintf(cmd, sizeof cmd, "SET ring_color %s", color);
      call(cmd);
    }
    break;
  }
  case 8:
    presets_menu();
    break;
  default:
    break;
  }
}
static int service(const char *operation) {
  char *args[] = {"rc-service", "capsule-controls", (char *)operation, NULL};
  return run_cmd(args, 12);
}
static void autostart(bool enabled) {
  char *args[] = {"rc-update", enabled ? "add" : "del", "capsule-controls",
                  "default", NULL};
  run_cmd(args, 5);
}
static bool uninstall(void) {
  puts("Удалить твикер и восстановить команды автозапуска? Введите ДА:");
  char yes[40];
  if (!fgets(yes, sizeof yes, stdin) || strncmp(yes, "ДА", strlen("ДА")))
    return false;
  service("stop");
  if (!service("status")) {
    puts("Служба не остановилась; удаление отменено.");
    return false;
  }
  autostart(false);
  if (startup_restore() < 0) {
    puts("Не удалось восстановить автозапуск; удаление отменено.");
    return false;
  }
  const char *files[] = {CONFIG_PATH,
                         "/etc/init.d/capsule-controls",
                         "/usr/local/bin/capsule-tweaker",
                         SOCKET_PATH,
                         "/run/capsule-controls.lock",
                         "/etc/capsule-controls.json"};
  for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
    char p[MAXPATH];
    path_for(p, sizeof p, files[i]);
    unlink(p);
  }
  char data[MAXPATH];
  path_for(data, sizeof data, "/var/lib/capsule-controls");
  char *args[] = {"rm", "-rf", data, NULL};
  run_cmd(args, 3);
  sync();
  puts("Твикер удалён. Резервные копии стартовых файлов сохранены.");
  return true;
}
int menu_run(void) {
  if (geteuid() != 0) {
    puts("Запустите capsule-tweaker от root на капсуле.");
    return 1;
  }
  char line[512];
  for (;;) {
    Config cfg;
    if (config_load(&cfg)) {
      puts("Ошибка в конфигурации.");
      return 1;
    }
    char response[512];
    bool live = !ipc_call("GET", response, sizeof response);
    puts("\n================================================\n КАПСУЛА МИНИ — "
         "ТВИКЕР НА C\n================================================");
    if (live) {
      int volume, muted, mode, effect, face, editing;
      sscanf(response, "OK %d %d %d %d %d %d", &volume, &muted, &mode, &effect,
             &face, &editing);
      printf("Громкость: %d/100 · %s\nЭкран: %s · мордашка: %s\nКольцо: %s%s\n",
             volume, muted || volume == 0 ? "МЬЮТ" : "звук включён",
             mode == 0   ? "доллар"
             : mode == 1 ? "часы"
                         : "мордашка",
             face_names[face], ring_effect_name(&cfg, effect),
             editing ? " (настройка кнопками)" : "");
    } else
      puts("Служба остановлена.");
    int sound = startup_sound(-1);
    printf("Звук при старте: %s\n", sound < 0 ? "команда не найдена"
                                    : sound   ? "включён"
                                              : "выключен");
    puts(
        "\n1. Громкость 0–100, шаг 5\n2. Мьют\n3. Экран: доллар / часы / "
        "мордашка\n4. Настройки экрана и мордашки\n5. Звук при старте\n6. "
        "Включить / выключить службу и автозапуск\n7. Перезапустить службу\n8. "
        "Диагностика\n9. Удалить твикер\n10. Настройки кольца\n0. Выход");
    int choice = answer(">", 0, 10, 1);
    if (choice == 0 || feof(stdin))
      return 0;
    switch (choice) {
    case 1: {
      int v = answer("Громкость", 0, 100, 5);
      if (v != -999) {
        char cmd[32];
        snprintf(cmd, sizeof cmd, "VOL %d", v);
        call(cmd);
      }
      break;
    }
    case 2:
      call("MUTE");
      break;
    case 3: {
      int v = answer("0 — доллар, 1 — часы, 2 — мордашка", 0, 2, 1);
      if (v != -999) {
        char cmd[32];
        snprintf(cmd, sizeof cmd, "MODE %d", v);
        call(cmd);
      }
      break;
    }
    case 4:
      screen_menu();
      break;
    case 5: {
      int v = answer("0 — выключить, 1 — включить", 0, 1, 1);
      if (v != -999)
        printf("Изменено команд: %d. Вступит в силу при следующем старте.\n",
               startup_sound(v));
      break;
    }
    case 6:
      if (live) {
        service("stop");
        autostart(false);
      } else {
        autostart(true);
        if (service("start"))
          puts("Не удалось запустить службу.");
      }
      break;
    case 7:
      if (service("restart"))
        puts("Не удалось перезапустить службу.");
      break;
    case 8: {
      puts(live ? response : "Служба недоступна.");
      char path[MAXPATH];
      path_for(path, sizeof path, RATE_PATH);
      if (read_text(path, line, sizeof line) > 0)
        printf("Курс: %s", line);
      puts("Журнал: logread | grep capsule-tweaker (или /var/log/messages).");
      break;
    }
    case 9:
      if (uninstall())
        return 0;
      break;
    case 10:
      ring_menu();
      break;
    default:
      break;
    }
    printf("\nEnter — в меню...");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin))
      return 0;
  }
}
