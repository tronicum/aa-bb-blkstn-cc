/* =====================================================================
 * dd.c - DD v0.2 headless: 3D ASCII Art Flag Demo for the terminal
 * (Klemmbaustein Kosmokratie)
 *
 * SPDX-License-Identifier: CC0-1.0
 * License: CC0 1.0 Universal (Public Domain Dedication) - applies to
 * THIS CODE. To the extent possible under law, the author has waived
 * all copyright and related rights to this work.
 * https://creativecommons.org/publicdomain/zero/1.0/
 *
 * Rendering INSPIRED BY AAlib (Jan Hubicka et al.,
 * https://aa-project.sourceforge.net/aalib/) - no AAlib code is used,
 * only the luminance-to-glyph idea, reimplemented from scratch.
 * The 3D wave is a port of the voxel wind physics from the HTML demo:
 * per-column sine displacement + crest/trough shading, hoist fixed.
 *
 * Dependencies:
 *   - libcurl (MIT/X derivative)          fetches PNGs + country list
 *   - stb_image.h (Public Domain/MIT)     decodes PNGs, single header,
 *     auto-downloaded by `make` from https://github.com/nothings/stb
 *   - Flag PNGs & country list: Public Domain, https://flagcdn.com
 *
 * Build:   make          (or: cc -std=c11 -O2 -o dd dd.c -lcurl -lm)
 * Run:     ./dd [-m] [-s] [-a] [period-seconds, default 6]
 *          -m / --mono    monochrom, wie das AAlib-Original
 *          -s / --static  keine 3D-Welle (flach, wie v0.1)
 *          -a / --audio   Chiptune-Loop. macOS: nativ via CoreAudio
 *                         (EIN Binary, kein Subprozess, keine Datei -
 *                         demoszene-konform). Linux: aplay-Fallback.
 * Keys:    SPACE pause (Welle weht weiter), ENTER next, Q quit
 *
 * Requires a truecolor-capable terminal (any modern one).
 * ===================================================================== */

#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
/* macOS/Darwin versteckt bei striktem POSIX-Makro BSD-Symbole wie
 * TIOCGWINSZ - _DARWIN_C_SOURCE macht sie wieder sichtbar. */
#define _DARWIN_C_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
/* stb_image.h ist Fremdcode (Public Domain) und wirft mit clang
 * -Wunused-function - Warnungen nur fuer diesen Include stummschalten,
 * damit -Werror in der CI sauber bleibt. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------------ */
/* Konfiguration                                                       */
/* ------------------------------------------------------------------ */

#define BASE_PNG    "https://flagcdn.com/w160/%s.png"
#define BASE_LIST   "https://flagcdn.com/en/codes.json"
#define RAMP        " .:-=+*x%#@"         /* dunkel -> hell            */
#define CHAR_ASPECT 0.5                    /* Terminalzelle B/H         */
#define MAX_FLAGS   512
#define FRAME_MS    70                     /* ~14 FPS Wellen-Animation  */
#define WAVE_FREQ   0.25                   /* Phase pro Spalte          */
#define WAVE_SPEED  0.15                   /* Phase pro Frame           */

typedef struct { char code[8]; char name[64]; } Flag;

/* ------------------------------------------------------------------ */
/* HTTP (libcurl -> Speicherpuffer)                                    */
/* ------------------------------------------------------------------ */

typedef struct { unsigned char *data; size_t len; } Buf;

static size_t buf_write(void *src, size_t sz, size_t n, void *userp)
{
    Buf *b = (Buf *)userp;
    size_t add = sz * n;
    unsigned char *p = realloc(b->data, b->len + add);
    if (!p) return 0;
    memcpy(p + b->len, src, add);
    b->data = p;
    b->len += add;
    return add;
}

static bool http_get(const char *url, Buf *out)
{
    out->data = NULL;
    out->len = 0;
    CURL *c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "dd-ascii-flag-demo/0.2");
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || status != 200) {
        free(out->data);
        out->data = NULL;
        out->len = 0;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Mini-JSON-Parser fuer das flache codes.json ({"cc":"Name", ...})    */
/* Bewusst kein JSON-Lib-Dependency: das Format ist trivial.           */
/* ------------------------------------------------------------------ */

static bool code_wanted(const char *code)
{
    size_t n = strlen(code);
    if (n == 2 && code[0] >= 'a' && code[0] <= 'z'
               && code[1] >= 'a' && code[1] <= 'z') return true;
    if (strncmp(code, "gb-", 3) == 0) return true;   /* UK Home Nations */
    return false;
}

static int parse_codes(const unsigned char *json, size_t len,
                       Flag *flags, int max)
{
    int count = 0;
    size_t i = 0;
    while (i < len && count < max) {
        while (i < len && json[i] != '"') i++;
        if (i >= len) break;
        size_t ks = ++i;
        while (i < len && json[i] != '"') i++;
        if (i >= len) break;
        size_t ke = i++;

        while (i < len && (json[i] == ' ' || json[i] == ':'
                        || json[i] == '\n' || json[i] == '\r'
                        || json[i] == '\t')) i++;
        if (i >= len || json[i] != '"') continue;
        size_t vs = ++i;
        while (i < len && json[i] != '"') i++;
        if (i >= len) break;
        size_t ve = i++;

        size_t klen = ke - ks, vlen = ve - vs;
        if (klen == 0 || klen >= sizeof flags->code) continue;
        if (vlen == 0) continue;
        if (vlen >= sizeof flags->name) vlen = sizeof flags->name - 1;

        char key[8] = {0};
        memcpy(key, json + ks, klen);
        if (!code_wanted(key)) continue;

        memcpy(flags[count].code, key, klen + 1);
        memcpy(flags[count].name, json + vs, vlen);
        flags[count].name[vlen] = '\0';
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Terminal-Handling (raw mode, alt screen)                            */
/* ------------------------------------------------------------------ */

static struct termios g_saved_tio;
static bool g_tio_saved = false;

static void term_restore(void)
{
    if (g_tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
    fputs("\x1b[?25h\x1b[?1049l", stdout);    /* Cursor an, alt screen aus */
    fflush(stdout);
}

static void audio_stop(void);   /* forward, definiert im Audio-Teil */

static void on_signal(int sig)
{
    (void)sig;
    audio_stop();
    term_restore();
    _exit(0);
}

static void term_setup(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) == 0) {
        g_tio_saved = true;
        struct termios raw = g_saved_tio;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    atexit(term_restore);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fputs("\x1b[?1049h\x1b[?25l", stdout);    /* alt screen, Cursor aus */
}

static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 4)
        return ws.ws_col;
    return 80;
}

static int term_rows(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 6)
        return ws.ws_row;
    return 24;
}

/* Liefert Taste oder 0; wartet max. ms Millisekunden. */
static int poll_key(int ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        unsigned char c = 0;
        if (read(STDIN_FILENO, &c, 1) == 1) return c;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Anzeige-Zustand + Zell-Grid-Cache                                   */
/*                                                                     */
/* Das Quellbild wird einmal pro Groesse auf ein Zell-Grid (RGB je     */
/* Terminalzelle) heruntergerechnet; die 3D-Welle verschiebt dann nur  */
/* noch Zellen - so bleibt die Animation billig.                       */
/* ------------------------------------------------------------------ */

static unsigned char *g_rgba = NULL;         /* dekodiertes Quellbild   */
static int  g_iw = 0, g_ih = 0;
static Flag g_cur;
static int  g_shown = 0, g_total = 0;
static bool g_mono = false, g_static = false, g_audio = false;
static bool g_audio_ok = false;              /* Sound-Engine laeuft     */

static unsigned char *g_grid = NULL;         /* cols*rows*4 (rgb+alpha) */
static int  g_gcols = 0, g_grows = 0;

static char  *g_fb = NULL;                   /* Frame-Ausgabepuffer     */
static size_t g_fb_cap = 0;

static void grid_build(int cols, int rows)
{
    free(g_grid);
    g_grid = malloc((size_t)cols * rows * 4);
    if (!g_grid) { g_gcols = g_grows = 0; return; }
    g_gcols = cols;
    g_grows = rows;

    for (int y = 0; y < rows; y++) {
        int y0 = y * g_ih / rows, y1 = (y + 1) * g_ih / rows;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < cols; x++) {
            int x0 = x * g_iw / cols, x1 = (x + 1) * g_iw / cols;
            if (x1 <= x0) x1 = x0 + 1;

            long r = 0, g = 0, b = 0, a = 0, n = 0;   /* Box-Filter */
            for (int py = y0; py < y1; py++) {
                const unsigned char *row = g_rgba + ((size_t)py * g_iw + x0) * 4;
                for (int px = x0; px < x1; px++, row += 4) {
                    r += row[0]; g += row[1]; b += row[2]; a += row[3];
                    n++;
                }
            }
            unsigned char *cell = g_grid + ((size_t)y * cols + x) * 4;
            cell[0] = (unsigned char)(r / n);
            cell[1] = (unsigned char)(g / n);
            cell[2] = (unsigned char)(b / n);
            cell[3] = (unsigned char)(a / n);
        }
    }
}

/* Groesse so waehlen, dass Flagge + Wellenhub in das Terminal passen. */
static void fit_grid(int *out_cols, int *out_rows, int *out_amp)
{
    int cols = term_cols() - 4;
    if (cols > g_iw) cols = g_iw;             /* nicht hochskalieren    */
    if (cols < 4) cols = 4;

    double ratio = ((double)g_ih / g_iw) * CHAR_ASPECT;
    int rows = (int)(cols * ratio + 0.5);

    int amp = g_static ? 0 : rows / 6 + 1;    /* Wellenhub in Zeilen    */
    int avail = term_rows() - 5 - 2 * amp;    /* Kopf/Fuss + Hub        */
    if (avail < 2) avail = 2;

    if (rows > avail) {                       /* Hoehen-Clamp           */
        rows = avail;
        cols = (int)(rows / ratio);
        if (cols < 4) cols = 4;
        amp = g_static ? 0 : rows / 6 + 1;
    }
    if (rows < 2) rows = 2;

    *out_cols = cols;
    *out_rows = rows;
    *out_amp = amp;
}

/* ------------------------------------------------------------------ */
/* 3D-Frame: Sinus-Displacement pro Spalte + Wellen-Shading            */
/* ------------------------------------------------------------------ */

static void fb_reserve(size_t need)
{
    if (need <= g_fb_cap) return;
    char *p = realloc(g_fb, need);
    if (p) { g_fb = p; g_fb_cap = need; }
}

static void render_frame(double phase)
{
    if (!g_rgba) return;

    int cols, rows, amp;
    fit_grid(&cols, &rows, &amp);
    if (cols != g_gcols || rows != g_grows) grid_build(cols, rows);
    if (!g_grid) return;

    int canvas = rows + 2 * amp;
    /* Worst case pro Zelle ~24 Byte Escape + Kopf/Fuss */
    fb_reserve((size_t)(canvas + 4) * ((size_t)cols + 2) * 26 + 512);
    if (!g_fb) return;
    char *o = g_fb;

    /* Kopf */
    char up[8] = {0};
    for (int i = 0; g_cur.code[i] && i < 7; i++)
        up[i] = (char)(g_cur.code[i] >= 'a' && g_cur.code[i] <= 'z'
                       ? g_cur.code[i] - 32 : g_cur.code[i]);
    o += sprintf(o, "\x1b[H\x1b[1;36m %s — %s\x1b[0m   \x1b[2m[%d/%d]\x1b[0m%s\x1b[K\n\x1b[K\n",
                 up, g_cur.name, g_shown, g_total,
                 g_audio_ok ? "   \x1b[1;35m♪ SOUND ON — LOS GEHT'S\x1b[0m" : "");

    const char *ramp = RAMP;
    int rlen = (int)strlen(ramp);

    for (int cy = 0; cy < canvas; cy++) {
        for (int x = 0; x < cols; x++) {
            double wf = cols > 1 ? (double)x / (cols - 1) : 0.0; /* Mast fix */
            double dy = amp * wf * sin(phase + x * WAVE_FREQ);
            int sy = cy - amp - (int)lround(dy);

            if (sy < 0 || sy >= rows) { *o++ = ' '; continue; }

            const unsigned char *cell = g_grid + ((size_t)sy * cols + x) * 4;
            if (cell[3] < 100) { *o++ = ' '; continue; }  /* Transparenz */

            /* Beleuchtung: Wellenkaemme hell, Taeler dunkel */
            double shade = g_static ? 1.0
                         : 1.0 + 0.35 * wf * cos(phase + x * WAVE_FREQ);
            double r = cell[0] * shade, g = cell[1] * shade, b = cell[2] * shade;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            double lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
            if (lum > 1.0) lum = 1.0;
            int gi = (int)(lum * (rlen - 1) + 0.5);

            if (g_mono)
                *o++ = ramp[gi];
            else
                o += sprintf(o, "\x1b[38;2;%d;%d;%dm%c",
                             (int)r, (int)g, (int)b, ramp[gi]);
        }
        o += sprintf(o, g_mono ? "\x1b[K\n" : "\x1b[0m\x1b[K\n");
    }

    o += sprintf(o, "\x1b[K\n\x1b[2m SPACE pause // ENTER next // Q quit"
                    " // CC0 // flags: flagcdn.com\x1b[0m\x1b[K\x1b[J");
    fwrite(g_fb, 1, (size_t)(o - g_fb), stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Flaggen laden                                                       */
/* ------------------------------------------------------------------ */

static bool show_flag(const Flag *f, int shown, int total)
{
    char url[128];
    snprintf(url, sizeof url, BASE_PNG, f->code);

    Buf png;
    if (!http_get(url, &png)) return false;

    int iw = 0, ih = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(png.data, (int)png.len,
                                                &iw, &ih, &comp, 4);
    free(png.data);
    if (!rgba) return false;

    if (g_rgba) stbi_image_free(g_rgba);
    g_rgba = rgba; g_iw = iw; g_ih = ih;
    g_cur = *f; g_shown = shown; g_total = total;
    g_gcols = g_grows = 0;                    /* Grid-Cache invalidieren */

    fputs("\x1b[2J", stdout);                 /* einmal sauber wischen   */
    return true;
}


/* ------------------------------------------------------------------ */
/* CHIPTUNE (optional, -a)                                             */
/* Tracker-Style: 32 Patterns a 4 Takte (~4 min, nahtlos geloopt),     */
/* je Pattern eigene Akkordfolge/Arp-Maske/Wellenform - prozedural.    */
/*                                                                     */
/* macOS: nativ ueber CoreAudio/AudioQueue - EIN Binary, kein          */
/*        Subprozess, keine Tempdatei (demoszene-konform).             */
/* Linux: WAV nach TMPDIR + aplay-Loop als pragmatischer Fallback.     */
/* ------------------------------------------------------------------ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define SYNTH_SR 44100

static int16_t *g_song = NULL;               /* gerenderte Loop (PCM)   */
static size_t   g_song_n = 0;

static void add_tone(float *b, size_t n, double t0, double dur,
                     double f, bool square, double vol)
{
    size_t s = (size_t)(t0 * SYNTH_SR), e = (size_t)((t0 + dur) * SYNTH_SR);
    if (e > n) e = n;
    for (size_t i = s; i < e; i++) {
        double t = (double)(i - s) / SYNTH_SR;
        double env = exp(-5.0 * t / dur);
        double ph = fmod(f * ((double)i / SYNTH_SR), 1.0);
        double w = square ? (ph < 0.5 ? 1.0 : -1.0)       /* Square   */
                          : (4.0 * fabs(ph - 0.5) - 1.0);  /* Triangle */
        b[i] += (float)(w * env * vol);
    }
}

static void add_kick(float *b, size_t n, double t0)
{
    size_t s = (size_t)(t0 * SYNTH_SR), e = s + (size_t)(0.14 * SYNTH_SR);
    if (e > n) e = n;
    double ph = 0.0;
    for (size_t i = s; i < e; i++) {
        double t = (double)(i - s) / SYNTH_SR;
        double f = 150.0 * exp(-8.0 * t) + 40.0;           /* Pitch-Sweep */
        ph += f / SYNTH_SR;
        b[i] += (float)(sin(2.0 * M_PI * ph) * exp(-18.0 * t) * 0.8);
    }
}

static void add_hat(float *b, size_t n, double t0, unsigned *seed)
{
    size_t s = (size_t)(t0 * SYNTH_SR), e = s + (size_t)(0.05 * SYNTH_SR);
    if (e > n) e = n;
    double prev = 0.0;
    for (size_t i = s; i < e; i++) {
        double t = (double)(i - s) / SYNTH_SR;
        *seed = *seed * 1103515245u + 12345u;              /* eigener LCG */
        double w = ((*seed >> 16) & 0x7fff) / 16383.5 - 1.0;
        double hp = w - prev;                              /* Hochpass    */
        prev = w;
        b[i] += (float)(hp * exp(-60.0 * t) * 0.5);
    }
}

static double midi_hz(int m)
{
    return 440.0 * pow(2.0, (m - 69) / 12.0);
}

/* 8 Akkordfolgen (MIDI-Grundton + Moll-Flag) - der Pattern-Pool        */
typedef struct { int root; int minor; } Ch;
static const Ch PROGS[8][4] = {
    {{57,1},{53,0},{48,0},{55,0}},   /* Am F  C  G  */
    {{57,1},{55,0},{53,0},{52,0}},   /* Am G  F  E  */
    {{48,0},{55,0},{57,1},{53,0}},   /* C  G  Am F  */
    {{50,1},{57,1},{52,0},{57,1}},   /* Dm Am E  Am */
    {{57,1},{48,0},{50,1},{52,0}},   /* Am C  Dm E  */
    {{48,0},{53,0},{57,1},{55,0}},   /* C  F  Am G  */
    {{55,0},{52,1},{48,0},{50,1}},   /* G  Em C  Dm */
    {{57,1},{60,0},{53,0},{55,0}}    /* Am C' F  G  */
};

static unsigned rnd(unsigned *st)
{
    *st = *st * 1103515245u + 12345u;
    return (*st >> 16) & 0x7fff;
}

#define PATTERNS 32                            /* 32 Tracks a 4 Takte   */

static bool synth_song(void)
{
    const double STEP = 60.0 / 125.0 / 4.0;    /* 16tel @ 125 BPM       */
    const int STEPS = 64;                      /* Steps pro Pattern     */
    size_t n = (size_t)((double)PATTERNS * STEPS * STEP * SYNTH_SR);
    float *buf = calloc(n, sizeof *buf);
    if (!buf) return false;

    for (int pat = 0; pat < PATTERNS; pat++) {
        unsigned ps = 0x9E3779B9u * (unsigned)(pat + 1) ^ 0xC0FFEEu;

        const Ch *prog = PROGS[rnd(&ps) % 8];
        unsigned mask  = (rnd(&ps) << 1) | 0x9249u;  /* Dichte sichern  */
        bool lead_sq   = (rnd(&ps) & 3) != 0;        /* meist Square    */
        int  lead_oct  = (pat % 4 == 3) ? 12 : 0;    /* jedes 4.: hoch  */
        int  hat_off   = (rnd(&ps) & 1) ? 2 : 3;     /* Groove-Variante */
        unsigned aseed = ps;

        for (int st = 0; st < STEPS; st++) {
            int bar = st / 16, s16 = st % 16;
            double t0 = ((double)pat * STEPS + st) * STEP;
            const Ch *c = &prog[bar];
            int tones[3] = { c->root, c->root + (c->minor ? 3 : 4),
                             c->root + 7 };

            if ((mask >> s16) & 1)                   /* Lead-Arp        */
                add_tone(buf, n, t0, STEP * 1.6,
                         midi_hz(tones[(st >> 1) % 3] + lead_oct
                                 + (s16 % 8 == 6 ? 12 : 0)),
                         lead_sq, 0.16);
            if (s16 % 2 == 0)                        /* Bass            */
                add_tone(buf, n, t0, STEP * 1.8,
                         midi_hz(c->root - 12 + (s16 % 8 == 4 ? 12 : 0)),
                         false, 0.30);
            if (s16 % 4 == 0) add_kick(buf, n, t0);  /* Drums           */
            if (s16 % 4 == hat_off) add_hat(buf, n, t0, &aseed);
        }
    }

    g_song = malloc(n * sizeof *g_song);
    if (!g_song) { free(buf); return false; }
    for (size_t i = 0; i < n; i++) {
        double v = buf[i] * 0.55;                    /* Master          */
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        g_song[i] = (int16_t)(v * 32767.0);
    }
    g_song_n = n;
    free(buf);
    return true;
}


#ifdef __APPLE__
/* ---- macOS: natives Streaming ueber AudioQueue (ein Binary) ------- */
#include <AudioToolbox/AudioToolbox.h>

static AudioQueueRef g_aq = NULL;
static size_t g_song_pos = 0;

static void aq_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)ud;
    int16_t *dst = (int16_t *)buf->mAudioData;
    size_t want = buf->mAudioDataBytesCapacity / sizeof(int16_t);
    for (size_t i = 0; i < want; i++) {
        dst[i] = g_song[g_song_pos++];
        if (g_song_pos >= g_song_n) g_song_pos = 0;        /* Endlos-Loop */
    }
    buf->mAudioDataByteSize = (UInt32)(want * sizeof(int16_t));
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void audio_start(void)
{
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = SYNTH_SR;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger
                        | kLinearPCMFormatFlagIsPacked;
    f.mBitsPerChannel   = 16;
    f.mChannelsPerFrame = 1;
    f.mBytesPerFrame    = 2;
    f.mFramesPerPacket  = 1;
    f.mBytesPerPacket   = 2;

    if (AudioQueueNewOutput(&f, aq_cb, NULL, NULL, NULL, 0, &g_aq) != 0)
        return;
    for (int i = 0; i < 3; i++) {                          /* 3 Puffer    */
        AudioQueueBufferRef b = NULL;
        if (AudioQueueAllocateBuffer(g_aq, 16384, &b) == 0 && b)
            aq_cb(NULL, g_aq, b);
    }
    if (AudioQueueStart(g_aq, NULL) == 0) g_audio_ok = true;
}

static void audio_stop(void)
{
    if (g_aq) {
        AudioQueueStop(g_aq, true);
        AudioQueueDispose(g_aq, true);
        g_aq = NULL;
    }
}

#else
/* ---- Linux: WAV nach TMPDIR + aplay-Loop (Fallback) --------------- */

static void wr_le32(FILE *fp, uint32_t v)
{
    fputc(v & 0xff, fp); fputc((v >> 8) & 0xff, fp);
    fputc((v >> 16) & 0xff, fp); fputc((v >> 24) & 0xff, fp);
}
static void wr_le16(FILE *fp, uint16_t v)
{
    fputc(v & 0xff, fp); fputc((v >> 8) & 0xff, fp);
}

static bool write_wav(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    fwrite("RIFF", 1, 4, fp); wr_le32(fp, (uint32_t)(36 + g_song_n * 2));
    fwrite("WAVEfmt ", 1, 8, fp); wr_le32(fp, 16);
    wr_le16(fp, 1);  wr_le16(fp, 1);                       /* PCM, mono  */
    wr_le32(fp, SYNTH_SR); wr_le32(fp, SYNTH_SR * 2);
    wr_le16(fp, 2);  wr_le16(fp, 16);
    fwrite("data", 1, 4, fp); wr_le32(fp, (uint32_t)(g_song_n * 2));
    for (size_t i = 0; i < g_song_n; i++)
        wr_le16(fp, (uint16_t)g_song[i]);
    fclose(fp);
    return true;
}

static pid_t g_audio_pid = -1;

static void audio_stop(void)
{
    if (g_audio_pid > 0) {
        kill(-g_audio_pid, SIGTERM);                       /* ganze Gruppe */
        waitpid(g_audio_pid, NULL, 0);
        g_audio_pid = -1;
    }
}

static void audio_start(void)
{
    static char wav[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(wav, sizeof wav, "%s/dd-chiptune.wav", tmp ? tmp : "/tmp");
    if (!write_wav(wav)) return;

    g_audio_pid = fork();
    if (g_audio_pid != 0) {                                /* Eltern      */
        if (g_audio_pid > 0) g_audio_ok = true;
        return;
    }

    setpgid(0, 0);                                         /* eigene Gruppe */
    for (;;) {
        pid_t p = fork();
        if (p == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
            execlp("aplay", "aplay", "-q", wav, (char *)NULL);
            _exit(127);                                    /* Player fehlt */
        }
        int st = 0;
        if (waitpid(p, &st, 0) < 0) _exit(1);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 127) _exit(1);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Demo-Loop                                                           */
/* ------------------------------------------------------------------ */

static void shuffle(Flag *f, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Flag tmp = f[i]; f[i] = f[j]; f[j] = tmp;
    }
}

int main(int argc, char **argv)
{
    int period = 6;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mono") == 0) {
            g_mono = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--static") == 0) {
            g_static = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--audio") == 0) {
            g_audio = true;
        } else {
            int p = atoi(argv[i]);
            if (p >= 1) period = p;
        }
    }

    srand((unsigned)time(NULL));
    curl_global_init(CURL_GLOBAL_DEFAULT);

    static Flag flags[MAX_FLAGS];
    int total = 0;

    Buf list;
    if (http_get(BASE_LIST, &list)) {
        total = parse_codes(list.data, list.len, flags, MAX_FLAGS);
        free(list.data);
    }
    if (total < 10) {
        fprintf(stderr, "dd: Laenderliste nicht ladbar (%s)\n", BASE_LIST);
        curl_global_cleanup();
        return 1;
    }
    shuffle(flags, total);

    if (g_audio && synth_song()) {
        audio_start();
        atexit(audio_stop);
    }

    term_setup();

    int pos = 0, shown = 0;
    bool paused = false, running = true;
    double phase = 0.0;

    while (running) {
        if (pos >= total) { shuffle(flags, total); pos = 0; }
        if (show_flag(&flags[pos++], ++shown, total)) {
            int elapsed_ms = 0;
            while (running && (paused || elapsed_ms < period * 1000)) {
                render_frame(phase);          /* passt sich jedem Resize an */
                int k = poll_key(FRAME_MS);
                if (!g_static) phase += WAVE_SPEED;
                if (!paused) elapsed_ms += FRAME_MS;
                if (k == 'q' || k == 'Q') running = false;
                else if (k == ' ') paused = !paused;   /* Welle weht weiter */
                else if (k == '\n' || k == '\r') break;
            }
        }
        /* nicht ladbare Flaggen werden einfach uebersprungen */
    }

    term_restore();
    curl_global_cleanup();
    return 0;
}
