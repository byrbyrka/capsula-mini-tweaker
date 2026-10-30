#ifndef CAPSULE_H
#define CAPSULE_H
#define _GNU_SOURCE
#include <sound/asound.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VERSION "1.0.0"
#define CONFIG_PATH "/etc/capsule-tweaker.conf"
#define STATE_PATH "/var/lib/capsule-controls/state.json"
#define RATE_PATH "/var/lib/capsule-controls/usd-native"
#define SOCKET_PATH "/run/capsule-controls.sock"
#define MAXPATH 512
#define KEY_COUNT 768
#define MAX_PRESETS 16
#define MAX_FRAMES 12
#define PRESET_TEXT 1024
enum { MODE_USD, MODE_CLOCK, MODE_FACE };
enum { FX_OFF, FX_RAINBOW, FX_FIRE, FX_SOLID, FX_COUNT };
#define FX_FRAMES FX_COUNT
enum { FEEDBACK_NONE, FEEDBACK_VOLUME, FEEDBACK_ACTION, FEEDBACK_FRONT };

typedef struct {
  char name[49];
  int effect, brightness, speed, frame_ms, count;
  unsigned char frames[MAX_FRAMES][36];
} RingPreset;
typedef struct {
  char input[MAXPATH], timezone[80], card[32], volume_control[80],
      mute_control[80];
  char ringled[MAXPATH], ring_color[7];
  int up_key, down_key, mute_key, action_keys[2], action_count, grab;
  int initial_mode, brightness, current_divider, frame_ms, face;
  int ring_brightness, ring_ms, ring_front, effect, ring_speed;
  int preset_count;
  RingPreset presets[MAX_PRESETS];
} Config;
typedef struct {
  int volume, muted, mode;
} State;
typedef struct {
  int fd;
  struct snd_ctl_elem_value volume, mute;
  unsigned volume_count, mute_count;
} Audio;
typedef struct {
  char message[MAXPATH], map[MAXPATH], brightness[MAXPATH], current[MAXPATH];
  char colon_top[MAXPATH], colon_bottom[MAXPATH];
  unsigned char glyphs[128];
  char last[5];
  int last_top, last_bottom, configured_brightness, configured_current;
  double next, retry;
  bool found;
} Display;
typedef struct {
  pid_t child;
  uint64_t last;
  double retry;
  int active_effect;
  int selected;
  double epoch, next;
} Ring;
typedef struct {
  bool down[KEY_COUNT], chord, dropped, blocked;
  double last_volume;
} Keys;
typedef struct {
  uint64_t micro;
  char date[16];
  time_t fetched;
  pid_t child;
  int fd;
  double due, deadline;
  size_t used;
  char xml[32768];
} Rate;
typedef struct App {
  Config cfg;
  State state;
  Audio audio;
  Display display;
  Ring ring;
  Keys keys;
  Rate rate;
  int input_fd, socket_fd, lock_fd;
  unsigned char input_bytes[4096];
  size_t input_used;
  bool dirty, config_dirty, editing;
  int old_effect, feedback;
  int ap2_state;
  double save_at, feedback_until, overlay_until, reconnect;
  char overlay[8];
} App;
extern const char *root_prefix;
extern const char *config_override;
double monotime(void);
void path_for(char *out, size_t cap, const char *path);
int read_text(const char *path, char *out, size_t cap);
int write_bytes(const char *path, const void *data, size_t length);
int atomic_text(const char *path, const char *text);
int mkdirs(const char *path);
pid_t spawn_cmd(char *const argv[], int output);
int run_cmd(char *const argv[], double timeout);
void stop_child(pid_t *pid);
void logmsg(int priority, const char *fmt, ...);
int json_int(const char *text, const char *key, int fallback);
int json_string(const char *text, const char *key, char *out, size_t cap);
int parse_int(const char *s, int *out);
void config_defaults(Config *cfg);
int config_load(Config *cfg);
int config_save(const Config *cfg);
int config_set(Config *cfg, const char *key, const char *value);
int config_validate(const Config *cfg);
int presets_validate(const Config *cfg);
int preset_parse(RingPreset *preset, const char *text);
int preset_format(const RingPreset *preset, char *text, size_t cap);
int preset_put(Config *cfg, int index, const RingPreset *preset);
int preset_delete(Config *cfg, int index);
int ring_effect_count(const Config *cfg);
const char *ring_effect_name(const Config *cfg, int effect);
void preset_frame(const RingPreset *preset, double elapsed,
                  unsigned char out[36]);
int preset_ipc(App *app, const char *request, char *reply, size_t cap,
               double now);
void presets_menu(void);
int presets_choices(void);
int state_load(State *state);
int state_save(const State *state);
int volume_step(int value);
int audio_open(Audio *audio, const Config *cfg, State *initial);
int audio_apply(Audio *audio, const State *state);
void audio_close(Audio *audio);
int display_open(Display *display);
void display_tick(App *app, double now);
void display_close(Display *display);
void ring_tick(App *app, double now);
void ring_close(App *app);
void ring_frame(const Config *cfg, int volume, bool muted, int feedback,
                unsigned char out[36]);
int input_open(const Config *cfg);
void key_event(App *app, int code, int value, double now);
void keys_drop(App *app);
int app_update(App *app, int volume, int muted, int mode, double now);
void app_edit(App *app, double now);
void app_change_effect(App *app, int effect, double now);
int rate_parse(const char *xml, uint64_t *micro, char date[16]);
void rate_startup(Rate *rate);
void rate_tick(Rate *rate, double now);
void rate_read(Rate *rate);
void rate_close(Rate *rate);
int ipc_open(void);
void ipc_receive(App *app, double now);
int ipc_call(const char *command, char *reply, size_t cap);
int service_run(void);
int startup_sound(int enabled);
int startup_prepare(bool first);
int startup_restore(void);
void legacy_stop(void);
int menu_run(void);
extern const char *effect_names[FX_COUNT];
extern const char *face_names[6];
#endif
