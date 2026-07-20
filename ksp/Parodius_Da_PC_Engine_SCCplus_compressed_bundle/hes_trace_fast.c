#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Music_Emu Music_Emu;
typedef const char *gme_err_t;
gme_err_t gme_open_file(const char *, Music_Emu **, int);
gme_err_t gme_load_m3u(Music_Emu *, const char *);
gme_err_t gme_start_track(Music_Emu *, int);
gme_err_t gme_play(Music_Emu *, int, short *);
void gme_ignore_silence(Music_Emu *, int);
void gme_delete(Music_Emu *);
int gme_track_count(Music_Emu *);

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s FILE.hes FILE.m3u TRACK_INDEX SECONDS\n", argv[0]);
        return 2;
    }
    int track = atoi(argv[3]);
    double seconds = atof(argv[4]);
    if (track < 0 || seconds <= 0) return 2;
    Music_Emu *emu = NULL;
    gme_err_t err = gme_open_file(argv[1], &emu, 6000);
    if (err) { fprintf(stderr, "open: %s\n", err); return 1; }
    err = gme_load_m3u(emu, argv[2]);
    if (err) { fprintf(stderr, "m3u: %s\n", err); gme_delete(emu); return 1; }
    if (track >= gme_track_count(emu)) {
        fprintf(stderr, "track %d outside 0..%d\n", track, gme_track_count(emu)-1);
        gme_delete(emu); return 1;
    }
    gme_ignore_silence(emu, 1);
    err = gme_start_track(emu, track);
    if (err) { fprintf(stderr, "start: %s\n", err); gme_delete(emu); return 1; }
    void (*marker)(uint32_t) = (void (*)(uint32_t))dlsym(RTLD_DEFAULT, "hes_trace_marker");
    if (!marker) { fprintf(stderr, "hes_trace_marker not found; use LD_PRELOAD\n"); return 1; }
    const int stereo_samples_per_frame = 200; /* 100 stereo frames = 1/60 s at 6000 Hz */
    short samples[stereo_samples_per_frame];
    uint32_t frames = (uint32_t)(seconds * 60.0 + 0.5);
    for (uint32_t frame = 0; frame < frames; frame++) {
        marker(frame);
        err = gme_play(emu, stereo_samples_per_frame, samples);
        if (err) { fprintf(stderr, "play at frame %u: %s\n", frame, err); break; }
    }
    fprintf(stderr, "traced track %d for %u frames\n", track, frames);
    gme_delete(emu);
    return err ? 1 : 0;
}
