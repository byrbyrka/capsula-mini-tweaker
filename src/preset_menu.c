#include "capsule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int line(const char *prompt, char *out, size_t cap) {
  printf("%s: ", prompt);
  fflush(stdout);
  if (!fgets(out, (int)cap, stdin))
    return -1;
  if (!strchr(out, '\n') && !feof(stdin)) {
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {
    }
    puts("Слишком длинное значение.");
    return -1;
  }
  out[strcspn(out, "\r\n")] = 0;
  return *out ? 0 : -1;
}
static int number(const char *prompt, int lo, int hi) {
  char text[64];
  int n;
  if (line(prompt, text, sizeof text))
    return -1;
  if (parse_int(text, &n) || n < lo || n > hi) {
    puts("Недопустимое значение.");
    return -1;
  }
  return n;
}
static int request(const char *command, char *reply, size_t cap) {
  if (!ipc_call(command, reply, cap))
    return 0;
  puts("Не удалось выполнить операцию. Проверьте службу; завершите настройку "
       "кольца кнопками. Имена пресетов должны быть уникальными.");
  return -1;
}
int presets_choices(void) {
  for (int i = 0; i < FX_COUNT; i++)
    printf("%d — %s\n", i, effect_names[i]);
  char reply[PRESET_TEXT + 80];
  if (request("PRESETS", reply, sizeof reply))
    return -1;
  int count;
  if (sscanf(reply, "OK %d", &count) != 1 || count < 0 || count > MAX_PRESETS)
    return -1;
  char *names = strchr(reply, '\n');
  if (names)
    fputs(names + 1, stdout);
  return FX_COUNT + count;
}
static int get(int index, RingPreset *p) {
  char cmd[40], reply[PRESET_TEXT + 80];
  snprintf(cmd, sizeof cmd, "PRESET_GET %d", index);
  if (request(cmd, reply, sizeof reply))
    return -1;
  return preset_parse(p, reply + 3);
}
static int color(unsigned char out[3]) {
  char text[32];
  if (line("Цвет RRGGBB (000000 — погасить; Enter — отмена)", text,
           sizeof text))
    return -1;
  if (strlen(text) != 6 || strspn(text, "0123456789abcdefABCDEF") != 6) {
    puts("Нужно ровно шесть шестнадцатеричных цифр, например ff8000.");
    return -1;
  }
  unsigned long value = strtoul(text, NULL, 16);
  out[0] = (unsigned char)(value >> 16);
  out[1] = (unsigned char)(value >> 8);
  out[2] = (unsigned char)value;
  return 0;
}
static void frame_editor(unsigned char frame[36]) {
  while (!feof(stdin)) {
    puts("\nСветодиоды 0–11; передний центр — 6 до калибровки:");
    for (int i = 0; i < 12; i++)
      printf("%2d: %02x%02x%02x%s", i, frame[i * 3], frame[i * 3 + 1],
             frame[i * 3 + 2], i % 3 == 2 ? "\n" : "   ");
    int x = number(
        "0–11 — цвет диода; 12 — заливка; 13/14 — сдвиг; 15 — готово", 0, 15);
    if (x < 0 || x == 15)
      return;
    if (x == 13 || x == 14) {
      unsigned char old[36];
      memcpy(old, frame, 36);
      for (int i = 0; i < 12; i++)
        memcpy(frame + i * 3, old + ((i + (x == 13 ? 1 : 11)) % 12) * 3, 3);
    } else {
      unsigned char rgb[3];
      if (color(rgb))
        continue;
      if (x < 12)
        memcpy(frame + x * 3, rgb, 3);
      else
        for (int i = 0; i < 12; i++)
          memcpy(frame + i * 3, rgb, 3);
    }
  }
}
static void editor(RingPreset *p, int index) {
  for (;;) {
    printf("\nПресет: %s\nЭффект: %s · яркость %d%% · кадров %d · кадр %d мс\n",
           p->name,
           p->effect == FX_FRAMES || p->effect == FX_SOLID
               ? "свои кадры"
               : effect_names[p->effect],
           p->brightness, p->count, p->frame_ms);
    puts("1 — имя\n2 — тип эффекта\n3 — яркость\n4 — время оборота радуги\n"
         "5 — длительность кадра\n6 — цвета кадра\n7 — добавить копию кадра\n"
         "8 — удалить кадр\n9 — сохранить\n10 — сохранить и включить\n"
         "0 — отменить изменения");
    int x = number(">", 0, 10), value;
    if (x == 0 || feof(stdin))
      return;
    switch (x) {
    case 1: {
      char name[sizeof p->name];
      if (!line("Имя (до 48 байт; Enter — оставить)", name, sizeof name)) {
        RingPreset test = *p;
        strcpy(test.name, name);
        char record[PRESET_TEXT];
        if (!preset_format(&test, record, sizeof record))
          *p = test;
        else
          puts("Имя не должно содержать |, управляющие символы или пробелы по "
               "краям.");
      }
      break;
    }
    case 2:
      value =
          number("0 — выключено; 1 — радуга; 2 — огонь; 3 — свои кадры", 0, 3);
      if (value >= 0)
        p->effect = value == 3 ? FX_FRAMES : value;
      break;
    case 3:
      value = number("Яркость 0–100", 0, 100);
      if (value >= 0)
        p->brightness = value;
      break;
    case 4:
      value = number("Время оборота радуги, мс (500–30000)", 500, 30000);
      if (value >= 0)
        p->speed = value;
      break;
    case 5:
      value = number("Длительность кадра, мс (250–10000)", 250, 10000);
      if (value >= 0)
        p->frame_ms = value;
      break;
    case 6:
      value =
          p->count == 1 ? 1 : number("Номер кадра (начиная с 1)", 1, p->count);
      if (value > 0) {
        frame_editor(p->frames[value - 1]);
        p->effect = FX_FRAMES;
      }
      break;
    case 7:
      if (p->count == MAX_FRAMES)
        puts("Не больше 12 кадров в пресете.");
      else {
        memcpy(p->frames[p->count], p->frames[p->count - 1], 36);
        p->count++;
        p->effect = FX_FRAMES;
        printf("Добавлен кадр %d; его цвета можно изменить пунктом 6.\n",
               p->count);
      }
      break;
    case 8:
      if (p->count == 1)
        puts("В пресете должен остаться хотя бы один кадр.");
      else {
        value = number("Номер удаляемого кадра", 1, p->count);
        if (value > 0) {
          memmove(p->frames[value - 1], p->frames[value],
                  (size_t)(p->count - value) * 36);
          memset(p->frames[--p->count], 0, 36);
        }
      }
      break;
    case 9:
    case 10: {
      char record[PRESET_TEXT], cmd[PRESET_TEXT + 80], reply[PRESET_TEXT + 80];
      if (preset_format(p, record, sizeof record)) {
        puts("Проверьте параметры пресета.");
        break;
      }
      snprintf(cmd, sizeof cmd, "PRESET_PUT %d %s", index, record);
      if (request(cmd, reply, sizeof reply))
        break;
      puts("Пресет сохранён.");
      if (x == 10) {
        snprintf(cmd, sizeof cmd, "RING %d", FX_COUNT + index);
        if (!request(cmd, reply, sizeof reply))
          puts("Пресет выбран. При мьюте красный индикатор имеет приоритет.");
      }
      return;
    }
    default:
      break;
    }
  }
}
void presets_menu(void) {
  while (!feof(stdin)) {
    puts("\nСВОИ ПРЕСЕТЫ КОЛЬЦА (до 16 пресетов, до 12 кадров в каждом)");
    int total = presets_choices();
    if (total < 0)
      return;
    int count = total - FX_COUNT;
    puts("\n1 — создать\n2 — изменить\n3 — включить\n4 — удалить\n0 — назад");
    int x = number(">", 0, 4);
    if (x == 0 || feof(stdin))
      return;
    if (x == 1) {
      if (count == MAX_PRESETS) {
        puts("Сначала удалите один из пресетов.");
        continue;
      }
      RingPreset p = {.effect = FX_FRAMES,
                      .brightness = 50,
                      .speed = 3000,
                      .frame_ms = 500,
                      .count = 1};
      snprintf(p.name, sizeof p.name, "Пресет %d", count + 1);
      for (int i = 0; i < 12; i++) {
        p.frames[0][i * 3 + 1] = 255;
        p.frames[0][i * 3 + 2] = 219;
      }
      editor(&p, count);
    } else if (x >= 2 && x <= 4) {
      if (!count) {
        puts("Своих пресетов пока нет.");
        continue;
      }
      int id = number("Номер своего пресета из списка (начиная с 4)", FX_COUNT,
                      total - 1);
      if (id < 0)
        continue;
      if (x == 2) {
        RingPreset p;
        if (!get(id - FX_COUNT, &p))
          editor(&p, id - FX_COUNT);
      } else {
        if (x == 4 && number("Удалить? 1 — да, 0 — нет", 0, 1) != 1)
          continue;
        char cmd[40], reply[PRESET_TEXT + 80];
        snprintf(cmd, sizeof cmd, x == 3 ? "RING %d" : "PRESET_DEL %d",
                 x == 3 ? id : id - FX_COUNT);
        if (!request(cmd, reply, sizeof reply))
          puts(x == 3 ? "Пресет выбран." : "Пресет удалён.");
      }
    }
  }
}
