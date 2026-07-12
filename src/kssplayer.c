/*
 * Small, platform-independent KSS player for testing generated archives.
 * SDL2 supplies the audio device on macOS, Linux, and Windows; libkss supplies
 * the KSS emulator and this program only connects the two.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <conio.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#if defined(__clang__)
/* libkss and SDL2 both expose legacy Uint8/Uint32 typedefs. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#endif
#include <SDL.h>
#include "kss/kss.h"
#include "kssplay.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define DEFAULT_RATE 44100
#define DEFAULT_CHANNELS 1
#define DEFAULT_SECONDS 60
#define DEFAULT_FADE_SECONDS 5
#define MAX_RATE 384000
#define MAX_SECONDS 3600

typedef struct {
  const char *input;
  int song;
  int rate;
  int channels;
  int seconds;
  int fade_seconds;
  int loops;
  int quality;
  int info_only;
} Options;

typedef struct {
  KSSPLAY *player;
  uint32_t rate;
  uint8_t channels;
  uint64_t total_frames;
  uint64_t rendered_frames;
  int fade_seconds;
  int loops;
  int fade_started;
  SDL_atomic_t done;
} AudioState;

typedef struct {
  int active;
#if !defined(_WIN32)
  int original_flags;
  struct termios original_settings;
#endif
} TerminalState;

static volatile sig_atomic_t stop_requested;

static void handle_interrupt(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

static int terminal_prepare(TerminalState *terminal) {
  memset(terminal, 0, sizeof(*terminal));

#if defined(_WIN32)
  return 1;
#else
  {
    struct termios settings;

    if (!isatty(STDIN_FILENO)) {
      return 1;
    }
    if (tcgetattr(STDIN_FILENO, &terminal->original_settings) != 0) {
      return 0;
    }
    terminal->original_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (terminal->original_flags < 0) {
      return 0;
    }

    settings = terminal->original_settings;
    settings.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0) {
      return 0;
    }
    if (fcntl(STDIN_FILENO, F_SETFL, terminal->original_flags | O_NONBLOCK) != 0) {
      tcsetattr(STDIN_FILENO, TCSANOW, &terminal->original_settings);
      return 0;
    }
    terminal->active = 1;
  }
  return 1;
#endif
}

static void terminal_restore(TerminalState *terminal) {
  if (!terminal->active) {
    return;
  }

#if defined(_WIN32)
  (void)terminal;
#else
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal->original_settings);
  fcntl(STDIN_FILENO, F_SETFL, terminal->original_flags);
#endif
  terminal->active = 0;
}

static int terminal_poll_quit(const TerminalState *terminal) {
#if defined(_WIN32)
  (void)terminal;
  while (_kbhit()) {
    if (_getch() == 27) {
      return 1;
    }
  }
  return 0;
#else
  unsigned char input[32];
  ssize_t count;
  size_t i;

  if (!terminal->active) {
    return 0;
  }
  count = read(STDIN_FILENO, input, sizeof(input));
  if (count <= 0) {
    return 0;
  }
  for (i = 0; i < (size_t)count; i++) {
    if (input[i] == 0x1b) {
      return 1;
    }
  }
  return 0;
#endif
}

static void usage(FILE *stream, const char *program) {
  fprintf(stream,
          "Usage: %s [options] FILE.KSS\n"
          "\n"
          "Play one KSS track through the default audio device.\n"
          "\n"
          "Options:\n"
          "  -sN, --song N       Song number (default: 0)\n"
          "  -pN, --seconds N    Play duration (default: 60)\n"
          "  -fN, --fade N       Fade duration in seconds (default: 5)\n"
          "  -lN, --loops N      Fade after this many loops (default: 1; 0=off)\n"
          "  -rN, --rate N       Requested sample rate (default: 44100)\n"
          "  -nN, --channels N   1 or 2 output channels (default: 1)\n"
          "  -qN, --quality N    0=low, 1=high (default: 1)\n"
          "  --info              Print archive metadata without playing\n"
          "  -h, --help          Show this help\n"
          "\n"
          "Short options accept either '-s3' or '-s 3'.\n",
          program);
}

static int parse_integer(const char *text, int minimum, int maximum, int *result) {
  char *end;
  long value;

  if (!text || !*text) {
    return 0;
  }

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno == ERANGE || *end != '\0' || value < minimum || value > maximum) {
    return 0;
  }

  *result = (int)value;
  return 1;
}

static int take_short_value(int argc, char **argv, int *index,
                            const char *argument, const char **value) {
  if (argument[2] != '\0') {
    *value = argument + 2;
    return 1;
  }
  if (*index + 1 >= argc) {
    return 0;
  }
  *value = argv[++*index];
  return 1;
}

/* Returns 1 for a value, 0 for a missing value, and -1 when the option does
 * not match name. */
static int take_long_value(int argc, char **argv, int *index,
                           const char *argument, const char *name,
                           const char **value) {
  size_t length = strlen(name);

  if (strcmp(argument, name) == 0) {
    if (*index + 1 >= argc) {
      return 0;
    }
    *value = argv[++*index];
    return 1;
  }
  if (strncmp(argument, name, length) == 0 && argument[length] == '=') {
    *value = argument + length + 1;
    return (*value)[0] != '\0';
  }
  return -1;
}

static int set_option_value(char option, const char *value, Options *options) {
  switch (option) {
  case 's':
    if (!parse_integer(value, 0, 255, &options->song)) {
      fprintf(stderr, "error: invalid song number: %s\n", value);
      return 0;
    }
    break;
  case 'p':
    if (!parse_integer(value, 1, MAX_SECONDS, &options->seconds)) {
      fprintf(stderr, "error: invalid duration: %s\n", value);
      return 0;
    }
    break;
  case 'f':
    if (!parse_integer(value, 0, MAX_SECONDS, &options->fade_seconds)) {
      fprintf(stderr, "error: invalid fade duration: %s\n", value);
      return 0;
    }
    break;
  case 'l':
    if (!parse_integer(value, 0, 256, &options->loops)) {
      fprintf(stderr, "error: invalid loop count: %s\n", value);
      return 0;
    }
    break;
  case 'r':
    if (!parse_integer(value, 8000, MAX_RATE, &options->rate)) {
      fprintf(stderr, "error: invalid sample rate: %s\n", value);
      return 0;
    }
    break;
  case 'n':
    if (!parse_integer(value, 1, 2, &options->channels)) {
      fprintf(stderr, "error: channels must be 1 or 2\n");
      return 0;
    }
    break;
  case 'q':
    if (!parse_integer(value, 0, 1, &options->quality)) {
      fprintf(stderr, "error: quality must be 0 or 1\n");
      return 0;
    }
    break;
  default:
    fprintf(stderr, "error: unknown option: -%c\n", option);
    return 0;
  }
  return 1;
}

static int parse_options(int argc, char **argv, Options *options) {
  int i;

  memset(options, 0, sizeof(*options));
  options->rate = DEFAULT_RATE;
  options->channels = DEFAULT_CHANNELS;
  options->seconds = DEFAULT_SECONDS;
  options->fade_seconds = DEFAULT_FADE_SECONDS;
  options->loops = 1;
  options->quality = 1;

  for (i = 1; i < argc; i++) {
    const char *argument = argv[i];
    const char *value = NULL;
    int matched;

    if (argument[0] != '-' || strcmp(argument, "-") == 0) {
      if (options->input) {
        fprintf(stderr, "error: only one input file is supported\n");
        return 0;
      }
      options->input = argument;
      continue;
    }

    if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
      usage(stdout, argv[0]);
      exit(0);
    }
    if (strcmp(argument, "--info") == 0) {
      options->info_only = 1;
      continue;
    }

    if (argument[1] == '-') {
      const char *names[] = {"--song", "--seconds", "--fade", "--loops",
                             "--rate", "--channels", "--quality"};
      const char short_names[] = {'s', 'p', 'f', 'l', 'r', 'n', 'q'};
      size_t option_index;

      matched = -1;
      for (option_index = 0; option_index < sizeof(names) / sizeof(names[0]);
           option_index++) {
        matched = take_long_value(argc, argv, &i, argument,
                                  names[option_index], &value);
        if (matched != -1) {
          if (matched == 0) {
            fprintf(stderr, "error: %s needs a value\n", argument);
            return 0;
          }
          if (!set_option_value(short_names[option_index], value, options)) {
            return 0;
          }
          break;
        }
      }
      if (matched == -1) {
        fprintf(stderr, "error: unknown option: %s\n", argument);
        return 0;
      }
      continue;
    }

    if (argument[1] != 's' && argument[1] != 'p' && argument[1] != 'f' &&
        argument[1] != 'l' && argument[1] != 'r' && argument[1] != 'n' &&
        argument[1] != 'q') {
      fprintf(stderr, "error: unknown option: %s\n", argument);
      return 0;
    }
    if (!take_short_value(argc, argv, &i, argument, &value) ||
        !set_option_value(argument[1], value, options)) {
      if (!value) {
        fprintf(stderr, "error: option %s needs a value\n", argument);
      }
      return 0;
    }
  }

  if (!options->input) {
    fprintf(stderr, "error: no KSS input file specified\n");
    return 0;
  }
  if (options->fade_seconds > options->seconds) {
    options->fade_seconds = options->seconds;
  }
  return 1;
}

static void print_info(const KSS *kss) {
  printf("id: %.8s\n", kss->idstr);
  printf("title: %.*s\n", KSS_TITLE_MAX, kss->title);
  printf("format: %s\n", kss->kssx ? "KSSX" : "KSCC");
  printf("load: $%04X-$%04X\n", kss->load_adr,
         (unsigned)(kss->load_adr + kss->load_len - 1));
  printf("init: $%04X\n", kss->init_adr);
  printf("play: $%04X\n", kss->play_adr);
  printf("banks: %u (%s)\n", kss->bank_num,
         kss->bank_mode == KSS_8K ? "8K" : "16K");
  printf("songs: %u-%u\n", kss->trk_min, kss->trk_max);
  printf("info records: %u\n", kss->info_num);
}

static void audio_callback(void *userdata, Uint8 *stream, int length) {
  AudioState *state = (AudioState *)userdata;
  const int bytes_per_frame = state->channels * (int)sizeof(int16_t);
  int frames;
  uint64_t remaining;

  SDL_memset(stream, 0, (size_t)length);
  if (SDL_AtomicGet(&state->done) || length < bytes_per_frame) {
    return;
  }

  remaining = state->total_frames - state->rendered_frames;
  frames = length / bytes_per_frame;
  if ((uint64_t)frames > remaining) {
    frames = (int)remaining;
  }
  if (frames <= 0) {
    SDL_AtomicSet(&state->done, 1);
    return;
  }

  KSSPLAY_calc(state->player, (int16_t *)stream, (uint32_t)frames);
  state->rendered_frames += (uint64_t)frames;

  if (!state->fade_started && state->fade_seconds > 0) {
    uint64_t fade_frames = (uint64_t)state->fade_seconds * state->rate;
    int loop_reached = state->loops > 0 &&
                       KSSPLAY_get_loop_count(state->player) >= state->loops;
    int time_reached = state->rendered_frames + fade_frames >= state->total_frames;

    if (loop_reached || time_reached) {
      KSSPLAY_fade_start(state->player,
                         (uint32_t)state->fade_seconds * 1000u);
      state->fade_started = 1;
    }
  }

  if (state->rendered_frames >= state->total_frames) {
    SDL_AtomicSet(&state->done, 1);
  }
}

static int play_audio(const Options *options, KSS *kss) {
  SDL_AudioSpec desired;
  SDL_AudioSpec actual;
  SDL_AudioDeviceID device = 0;
  AudioState state;
  TerminalState terminal;
  const char *driver;
  int result = 0;

  memset(&state, 0, sizeof(state));
  memset(&terminal, 0, sizeof(terminal));
  SDL_AtomicSet(&state.done, 0);

  if (SDL_Init(SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "error: SDL audio initialization failed: %s\n",
            SDL_GetError());
    return 0;
  }

  SDL_zero(desired);
  desired.freq = options->rate;
  desired.format = AUDIO_S16SYS;
  desired.channels = (Uint8)options->channels;
  desired.samples = 1024;
  desired.callback = audio_callback;
  desired.userdata = &state;

  device = SDL_OpenAudioDevice(NULL, 0, &desired, &actual,
                               SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                   SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
  if (!device) {
    fprintf(stderr, "error: cannot open an audio device: %s\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return 0;
  }
  if (actual.format != AUDIO_S16SYS || actual.channels < 1 || actual.channels > 2) {
    fprintf(stderr, "error: audio device did not accept 16-bit mono/stereo PCM\n");
    goto cleanup;
  }

  state.rate = (uint32_t)actual.freq;
  state.channels = actual.channels;
  state.total_frames = (uint64_t)state.rate * (uint64_t)options->seconds;
  state.fade_seconds = options->fade_seconds;
  state.loops = options->loops;
  state.player = KSSPLAY_new(state.rate, state.channels, 16);
  if (!state.player) {
    fprintf(stderr, "error: could not create KSS player\n");
    goto cleanup;
  }
  if (KSSPLAY_set_data(state.player, kss) != 0) {
    fprintf(stderr, "error: could not attach KSS data\n");
    goto cleanup;
  }
  KSSPLAY_reset(state.player, (uint32_t)options->song, 0);
  KSSPLAY_set_device_quality(state.player, KSS_DEVICE_PSG,
                             (uint32_t)options->quality);
  KSSPLAY_set_device_quality(state.player, KSS_DEVICE_SCC,
                             (uint32_t)options->quality);
  KSSPLAY_set_device_quality(state.player, KSS_DEVICE_OPLL,
                             (uint32_t)options->quality);
  if (state.channels == 2) {
    KSSPLAY_set_device_pan(state.player, KSS_DEVICE_PSG, -32);
    KSSPLAY_set_device_pan(state.player, KSS_DEVICE_SCC, 32);
    state.player->opll_stereo = 1;
    KSSPLAY_set_channel_pan(state.player, KSS_DEVICE_OPLL, 0, 1);
    KSSPLAY_set_channel_pan(state.player, KSS_DEVICE_OPLL, 1, 2);
    KSSPLAY_set_channel_pan(state.player, KSS_DEVICE_OPLL, 2, 1);
    KSSPLAY_set_channel_pan(state.player, KSS_DEVICE_OPLL, 3, 2);
    KSSPLAY_set_channel_pan(state.player, KSS_DEVICE_OPLL, 4, 1);
    KSSPLAY_set_channel_pan(state.player, KSS_DEVICE_OPLL, 5, 2);
  }

  driver = SDL_GetCurrentAudioDriver();
  fprintf(stderr, "playing song %d at %u Hz, %u channel(s) via %s\n",
          options->song, state.rate, state.channels,
          driver ? driver : "SDL audio");
  if (!terminal_prepare(&terminal)) {
    fprintf(stderr, "warning: Escape key handling is unavailable on this terminal\n");
  }
  fprintf(stderr, "press Escape or Ctrl-C to stop\n");

  SDL_PauseAudioDevice(device, 0);
  while (!SDL_AtomicGet(&state.done) && !stop_requested) {
    if (terminal_poll_quit(&terminal)) {
      SDL_AtomicSet(&state.done, 1);
    }
    SDL_Delay(25);
  }
  SDL_PauseAudioDevice(device, 1);
  result = 1;

cleanup:
  terminal_restore(&terminal);
  if (device) {
    SDL_CloseAudioDevice(device);
  }
  KSSPLAY_delete(state.player);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return result;
}

int main(int argc, char **argv) {
  Options options;
  KSS *kss;
  int result;
  void (*previous_interrupt_handler)(int);

  if (!parse_options(argc, argv, &options)) {
    usage(stderr, argv[0]);
    return 2;
  }

  kss = KSS_load_file((char *)options.input);
  if (!kss) {
    fprintf(stderr, "error: cannot load KSS file: %s\n", options.input);
    return 1;
  }

  if (options.song < kss->trk_min || options.song > kss->trk_max) {
    fprintf(stderr, "error: song %d is outside archive range %u-%u\n",
            options.song, kss->trk_min, kss->trk_max);
    KSS_delete(kss);
    return 1;
  }

  if (options.info_only) {
    print_info(kss);
    KSS_delete(kss);
    return 0;
  }

  stop_requested = 0;
  previous_interrupt_handler = signal(SIGINT, handle_interrupt);
  if (previous_interrupt_handler == SIG_ERR) {
    fprintf(stderr, "warning: Ctrl-C handling is unavailable\n");
    result = play_audio(&options, kss) ? 0 : 1;
  } else {
    result = play_audio(&options, kss) ? 0 : 1;
    signal(SIGINT, previous_interrupt_handler);
  }
  KSS_delete(kss);
  return result;
}
