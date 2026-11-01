/* ocr_page - read a whole screenshot the way Rubi does, natively, and report timing / CPU / memory.
 *
 *   ocr_page FRAME.ppm|pgm --models DIR [--lm FILE] [--tier S|N|L] [--no-cascade] [--no-lm]
 *            [--region x,y,w,h] [--all] [--grow PX] [--repeat N] [--lm-alpha A --lm-beta B --lm-flags F]
 *
 * Path (Rubi's, as eval/pagepath.py models it): the textspot8 spotter on the frame (or on --region at the frame's
 * scale) -> every horizontal dialogue box (--all: every horizontal box) cut from the grey frame, VOCR_INVERT when the
 * spotter calls it dark -> W8I greedy with the top 8 kept (head gate / two-level head when the blob carries them)
 * -> the counter re-decode (6 / 4 nats) -> beam 8 + the character LM (its shipped alpha / beta / flags) -> the cascade: a line whose greedy conf_min
 * is under 0.7 is read again by the next tier up and that tier's beam text is shown.
 *
 * Prints one JSON object on stdout. Single-threaded, like the console runtime, unless --threads N (Windows): then
 * the finder's rows and the recognizer's matrix products are split across N threads (bench/pool.h), same results.
 */
#define _POSIX_C_SOURCE 200112L
#include "vocr.h"
#include "vocr_lm.h"
#include "vocr_post.h"
#include "vocr_lines.h"
#include "vocr_textspot6.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include "pool.h"
#endif

#define TOPK 8
#define MAX_WIDTH 1024
#define CASCADE_CONF 700
#define MAX_REPEAT 1000

static double now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory (%zu B)\n", n);
        exit(2);
    }
    return p;
}

static size_t bytes_owned; /* blobs + arenas + workspaces this program allocated for the OCR */

static void *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long len;
    void *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    buf = xmalloc((size_t)len + 16);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return buf;
}

/* ---- PNM -> grey (77 R + 150 G + 29 B) >> 8, the finder's and spotter's rule */
static unsigned char *read_pnm_grey(const char *path, unsigned *w, unsigned *h)
{
    size_t n, i;
    unsigned char *f = (unsigned char *)read_file(path, &n), *p, *g;
    unsigned vals[3], k = 0, ch;
    if (!f || n < 2 || f[0] != 'P' || (f[1] != '5' && f[1] != '6')) {
        fprintf(stderr, "%s: not a binary PGM/PPM\n", path);
        exit(2);
    }
    ch = f[1] == '6' ? 3u : 1u;
    p = f + 2;
    while (k < 3) {
        while (p < f + n && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        if (*p == '#') {
            while (p < f + n && *p != '\n') p++;
            continue;
        }
        vals[k] = 0;
        while (p < f + n && *p >= '0' && *p <= '9') vals[k] = vals[k] * 10 + (unsigned)(*p++ - '0');
        k++;
    }
    p++;
    *w = vals[0];
    *h = vals[1];
    g = (unsigned char *)xmalloc((size_t)*w * *h);
    for (i = 0; i < (size_t)*w * *h; i++) {
        if (ch == 3) g[i] = (unsigned char)((77u * p[3 * i] + 150u * p[3 * i + 1] + 29u * p[3 * i + 2]) >> 8);
        else g[i] = p[i];
    }
    free(f);
    return g;
}

/* ---- one recognizer tier with its LM decoder */
typedef struct {
    char name;
    int loaded;
    void *blob;
    size_t blob_n;
    vocr_model m;
    vocr_ctx ctx;
    void *arena;
    size_t arena_n;
    vocr_lm_ctx dec;
    void *ws;
    size_t ws_n;
    int gate, two_level;
    double load_ms;
} tier_t;

static const char *BLOB_NAMES[3] = {"VOCR6.2_S_w8.vocr", "VOCR6.2_N_w8g2.vocr", "VOCR6.2_L_w8_2l.vocr"};
static tier_t tiers[3];
static const char *models_dir = "models";
static vocr_lm lm;
static int use_lm = 1;
static int vocr_mode = VOCR_MODE_W8I; /* --mode w8f: int8 weights, float activations (not what Rubi runs) */
static unsigned tile_frames, head_frames; /* --tile N / --head N: vocr_opts.tile_frames / head_frames (0: default) */
static vocr_lm_opts lo;

static tier_t *get_tier(int t)
{
    tier_t *T = &tiers[t];
    char path[1024];
    vocr_opts o;
    double t0;
    int rc;
    if (T->loaded) return T;
    t0 = now();
    T->name = "SNL"[t];
    snprintf(path, sizeof path, "%s/%s", models_dir, BLOB_NAMES[t]);
    T->blob = read_file(path, &T->blob_n);
    if (!T->blob || (rc = vocr_load(&T->m, T->blob, T->blob_n))) {
        fprintf(stderr, "%s: cannot load\n", path);
        exit(2);
    }
    memset(&o, 0, sizeof o);
    o.mode = vocr_mode;
    o.max_width = MAX_WIDTH;
    o.tile_frames = tile_frames;
    o.head_frames = head_frames;
    T->gate = vocr_has_gate(&T->m);
    T->two_level = vocr_has_two_level(&T->m);
    o.head_gate = T->gate ? VOCR_HEAD_GATE_ON : VOCR_HEAD_GATE_OFF;
    o.two_level = (unsigned)T->two_level;
    T->arena_n = vocr_arena_bytes(&T->m, &o);
    T->arena = xmalloc(T->arena_n);
    if ((rc = vocr_init(&T->ctx, &T->m, &o, T->arena, T->arena_n))) {
        fprintf(stderr, "vocr_init %c: %s\n", T->name, vocr_strerror(rc));
        exit(2);
    }
    if (use_lm) {
        T->ws_n = vocr_lm_workspace_bytes(&T->m, &lm, &lo);
        T->ws = xmalloc(T->ws_n);
        if ((rc = vocr_lm_init(&T->dec, &T->m, &lm, &lo, T->ws, T->ws_n))) {
            fprintf(stderr, "vocr_lm_init %c: %d\n", T->name, rc);
            exit(2);
        }
    }
    bytes_owned += T->blob_n + T->arena_n + T->ws_n;
    T->load_ms = (now() - t0) * 1e3;
    T->loaded = 1;
    return T;
}

typedef struct {
    char greedy[1024], beam[1024];
    float conf_min, conf_mean;
    unsigned frames, gated, escaped;
    double ms;
} read_t;

static float topv[(MAX_WIDTH / 8 + 1) * TOPK], blank[MAX_WIDTH / 8 + 1];
static uint16_t topc[(MAX_WIDTH / 8 + 1) * TOPK];

static void read_line(tier_t *T, const unsigned char *grey, unsigned W, const VocrTextspot6Box *b, read_t *r)
{
    static const vocr_post_opts red = {6000, 4000};
    vocr_image img;
    vocr_extra ex;
    double t0 = now();
    int rc;
    memset(&ex, 0, sizeof ex);
    memset(r, 0, sizeof *r);
    img.pixels = grey + (size_t)b->y * W + b->x;
    img.width = b->w;
    img.height = b->h;
    img.stride = W;
    img.format = VOCR_GREY8 | (b->dark ? VOCR_INVERT : 0);
    ex.cap_frames = MAX_WIDTH / 8 + 1;
    ex.topk = TOPK;
    ex.topk_logit = topv;
    ex.topk_class = topc;
    ex.blank_logit = blank;
    rc = vocr_run(&T->ctx, &img, r->greedy, sizeof r->greedy, &ex);
    if (rc && rc != VOCR_E_TEXT) {
        fprintf(stderr, "vocr_run %c: %s\n", T->name, vocr_strerror(rc));
        r->greedy[0] = 0;
    }
    r->conf_min = ex.conf_min;
    r->conf_mean = ex.conf_mean;
    r->frames = ex.frames;
    r->gated = ex.gated;
    r->escaped = ex.escaped;
    if (r->greedy[0]) {
        if (vocr_post_redecode(&T->m, &ex, &red, NULL) > 0 && !use_lm)
            vocr_post_greedy(&T->m, &ex, r->greedy, sizeof r->greedy, NULL);
        if (use_lm) vocr_lm_decode(&T->dec, &ex, r->beam, sizeof r->beam, NULL);
        else memcpy(r->beam, r->greedy, sizeof r->beam);
    }
    vocr_post_counters(r->beam);
    r->ms = (now() - t0) * 1e3;
}

static int line_conf(const read_t *r)
{
    double v;
    if (!r->greedy[0]) return 1000;
    v = (double)r->conf_min * 1000.0 + 0.5;
    return v <= 0 ? 0 : v >= 1000 ? 1000 : (int)v;
}

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", *s);
        else putchar(*s);
    }
    putchar('"');
}

int main(int argc, char **argv)
{
    const char *frame_path = NULL, *lm_path = NULL;
    int job = 1, cascade = 1, all = 0, repeat = 0, threads = 1, i, it, lm_alpha = -1, lm_beta = -1, lm_flags = -1;
    unsigned rx = 0, ry = 0, rw = 0, rh = 0, W, H, flags = 0, grow = 0;
    unsigned char *grey;
    void *ts_blob, *ts_arena, *lm_blob = NULL;
    size_t ts_n, ts_arena_n, lm_n = 0;
    vocr_textspot6 ts;
    VocrTextspot6Set set;
    VocrLinesImage im;
    static read_t first[VOCR_TEXTSPOT_MAX_BOXES], up[VOCR_TEXTSPOT_MAX_BOXES];
    static int used_up[VOCR_TEXTSPOT_MAX_BOXES], sel[VOCR_TEXTSPOT_MAX_BOXES];
    static double t_iter[MAX_REPEAT], t_spot[MAX_REPEAT];
    double t_start = now(), t0, spot_ms, total_ms, load_ms;
    char path[1024];
    unsigned n_sel = 0, frames_total = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc0;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc0, sizeof pmc0);
#endif

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--models") && i + 1 < argc) models_dir = argv[++i];
        else if (!strcmp(argv[i], "--lm") && i + 1 < argc) lm_path = argv[++i];
        else if (!strcmp(argv[i], "--tier") && i + 1 < argc) {
            const char *t = argv[++i];
            job = t[0] == 'S' ? 0 : t[0] == 'L' ? 2 : 1;
        } else if (!strcmp(argv[i], "--no-cascade")) cascade = 0;
        else if (!strcmp(argv[i], "--no-lm")) use_lm = 0;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            vocr_mode = !strcmp(m, "w8f") ? VOCR_MODE_W8F : VOCR_MODE_W8I;
        }
        else if (!strcmp(argv[i], "--grow") && i + 1 < argc) grow = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lm-alpha") && i + 1 < argc) lm_alpha = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lm-beta") && i + 1 < argc) lm_beta = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lm-flags") && i + 1 < argc) lm_flags = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tile") && i + 1 < argc) tile_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--head") && i + 1 < argc) head_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--region") && i + 1 < argc) {
            if (sscanf(argv[++i], "%u,%u,%u,%u", &rx, &ry, &rw, &rh) != 4) return 2;
        } else frame_path = argv[i];
    }
    if (!frame_path) {
        fprintf(stderr, "usage: ocr_page FRAME.ppm --models DIR [--lm FILE] [--tier S|N|L] [--no-cascade] "
                        "[--no-lm] [--region x,y,w,h] [--all] [--repeat N]\n");
        return 2;
    }
    if (repeat > MAX_REPEAT) repeat = MAX_REPEAT;
#ifdef _WIN32
    if (threads > 1) { /* --threads N: the finder's rows and the recognizer's products split across N threads */
        pool_start((unsigned)threads);
        vocr_set_parallel(pool_run, NULL, pool_ways());
        vocr_textspot6_set_parallel(pool_run, NULL, pool_ways());
    }
#endif
    if (!lm_path) use_lm = 0;

    t0 = now();
    grey = read_pnm_grey(frame_path, &W, &H);
    if (!rw) {
        rw = W;
        rh = H;
    }
    /* models: the spotter, the LM, the job's tier (the cascade tier loads when a line first needs it) */
    snprintf(path, sizeof path, "%s/%s", models_dir, "textspot8_t64n8c16_w8.vtsp");
    ts_blob = read_file(path, &ts_n);
    if (!ts_blob || vocr_textspot6_load(&ts, ts_blob, ts_n)) {
        fprintf(stderr, "%s: cannot load\n", path);
        return 2;
    }
    if (rw != W || rh != H) /* a region read at the scale the blob's rule gives the whole frame */
        flags = ((ts.half_above && W > ts.half_above) || (ts.half_above_h && H > ts.half_above_h))
                    ? VOCR_TEXTSPOT6_SCALE2 : VOCR_TEXTSPOT6_SCALE1;
    ts_arena_n = vocr_textspot6_frame_arena_bytes_flags(&ts, rw, rh, flags);
    ts_arena = xmalloc(ts_arena_n);
    bytes_owned += ts_n + ts_arena_n;
    if (use_lm) {
        lm_blob = read_file(lm_path, &lm_n);
        if (!lm_blob || vocr_lm_load(&lm, lm_blob, lm_n)) {
            fprintf(stderr, "%s: cannot load LM\n", lm_path);
            return 2;
        }
        bytes_owned += lm_n;
        /* docs/vocr6-lm.md "What ships": the game-text lm6 tiers at 600 / 2500 / BOS, ctclm.2's at 500 / 3000 / EOS;
         * beam 8, top 8, gate 8 nats and bracket 8 nats everywhere. --lm-alpha / --lm-beta / --lm-flags override. */
        lo.beam = 8;
        lo.topk = TOPK;
        if (strstr(lm_path, "lm6_")) {
            lo.alpha = 600;
            lo.beta = 2500;
            lo.flags = VOCR_LM_BOS;
        } else {
            lo.alpha = 500;
            lo.beta = 3000;
            lo.flags = VOCR_LM_EOS;
        }
        if (lm_alpha >= 0) lo.alpha = lm_alpha;
        if (lm_beta >= 0) lo.beta = lm_beta;
        if (lm_flags >= 0) lo.flags = (unsigned)lm_flags;
        lo.gate = 8000;
        lo.bracket = 8000;
    }
    get_tier(job);
    load_ms = (now() - t0) * 1e3;

    im.pixels = grey + (size_t)ry * W + rx;
    im.width = rw;
    im.height = rh;
    im.stride = W;
    im.bpp = VOCR_LINES_GREY;

    for (it = 0; it <= repeat; it++) { /* iteration 0 is the cold run; 1..repeat are timed warm runs */
        double ti = now();
        int rc;
        t0 = now();
        rc = vocr_textspot6_find(&ts, &im, flags, ts_arena, ts_arena_n, &set);
        spot_ms = (now() - t0) * 1e3;
        if (rc) {
            fprintf(stderr, "spotter: %d\n", rc);
            return 2;
        }
        n_sel = 0;
        frames_total = 0;
        for (i = 0; i < (int)set.count; i++) {
            VocrTextspot6Box b = set.boxes[i];
            if (b.vertical || (!all && b.tag != VOCR_TEXTSPOT6_DIALOGUE)) continue;
            b.x = (unsigned short)(b.x + rx);
            b.y = (unsigned short)(b.y + ry);
            if (grow) { /* diagnostic: widen the box on both sides, clipped to the frame */
                unsigned x0 = b.x > grow ? b.x - grow : 0, x1 = b.x + b.w + grow < W ? b.x + b.w + grow : W;
                b.x = (unsigned short)x0;
                b.w = (unsigned short)(x1 - x0);
            }
            sel[n_sel] = i;
            read_line(get_tier(job), grey, W, &b, &first[n_sel]);
            frames_total += first[n_sel].frames;
            used_up[n_sel] = 0;
            if (cascade && job < 2 && first[n_sel].greedy[0] && line_conf(&first[n_sel]) < CASCADE_CONF) {
                read_line(get_tier(job + 1), grey, W, &b, &up[n_sel]);
                frames_total += up[n_sel].frames;
                used_up[n_sel] = 1;
            }
            n_sel++;
        }
        total_ms = (now() - ti) * 1e3;
        if (it > 0) {
            t_iter[it - 1] = total_ms;
            t_spot[it - 1] = spot_ms;
        } else {
            t_iter[MAX_REPEAT - 1] = total_ms; /* keep the cold numbers apart */
            t_spot[MAX_REPEAT - 1] = spot_ms;
        }
    }

    printf("{\"frame\":");
    json_str(frame_path);
    printf(",\"width\":%u,\"height\":%u,\"region\":[%u,%u,%u,%u],\"spot_flags\":%u", W, H, rx, ry, rw, rh, flags);
    printf(",\"job_tier\":\"%c\",\"cascade\":%d,\"lm\":%d,\"threads\":%d", "SNL"[job], cascade, use_lm, threads);
    if (use_lm) printf(",\"lm_opts\":{\"alpha\":%d,\"beta\":%d,\"flags\":%u}", lo.alpha, lo.beta, lo.flags);
    printf(",\"load_ms\":%.3f,\"cold_total_ms\":%.3f,\"cold_spot_ms\":%.3f", load_ms,
           t_iter[MAX_REPEAT - 1], t_spot[MAX_REPEAT - 1]);
    printf(",\"boxes\":[");
    for (i = 0; i < (int)set.count; i++) {
        const VocrTextspot6Box *b = &set.boxes[i];
        printf("%s{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"tag\":%u,\"conf\":%u,\"dark\":%u,\"vertical\":%u}",
               i ? "," : "", b->x + rx, b->y + ry, b->w, b->h, b->tag, b->conf, b->dark, b->vertical);
    }
    printf("],\"lines\":[");
    for (i = 0; i < (int)n_sel; i++) {
        const read_t *r = &first[i];
        const char *shown = used_up[i] ? (up[i].greedy[0] ? up[i].beam : "") : r->beam;
        printf("%s{\"box\":%d,\"text\":", i ? "," : "", sel[i]);
        json_str(shown);
        printf(",\"greedy\":");
        json_str(r->greedy);
        printf(",\"beam\":");
        json_str(r->beam);
        printf(",\"conf_min\":%.4f,\"conf_mean\":%.4f,\"frames\":%u,\"gated\":%u,\"escaped\":%u,\"ms\":%.3f",
               (double)r->conf_min, (double)r->conf_mean, r->frames, r->gated, r->escaped, r->ms);
        if (used_up[i]) {
            printf(",\"cascade\":{\"tier\":\"%c\",\"greedy\":", "SNL"[job + 1]);
            json_str(up[i].greedy);
            printf(",\"beam\":");
            json_str(up[i].beam);
            printf(",\"conf_min\":%.4f,\"ms\":%.3f}", (double)up[i].conf_min, up[i].ms);
        }
        printf("}");
    }
    printf("],\"frames\":%u", frames_total);
    printf(",\"tiers\":[");
    for (i = 0, it = 0; i < 3; i++) {
        if (!tiers[i].loaded) continue;
        printf("%s{\"tier\":\"%c\",\"blob_bytes\":%zu,\"arena_bytes\":%zu,\"lm_ws_bytes\":%zu,\"gate\":%d,"
               "\"two_level\":%d,\"load_ms\":%.3f}", it++ ? "," : "", tiers[i].name, tiers[i].blob_n,
               tiers[i].arena_n, tiers[i].ws_n, tiers[i].gate, tiers[i].two_level, tiers[i].load_ms);
    }
    printf("],\"spotter_blob_bytes\":%zu,\"spotter_arena_bytes\":%zu,\"lm_bytes\":%zu,\"ocr_bytes_owned\":%zu",
           ts_n, ts_arena_n, lm_n, bytes_owned);
    if (repeat > 0) {
        printf(",\"warm\":{\"n\":%d,\"total_ms\":[", repeat);
        for (i = 0; i < repeat; i++) printf("%s%.4f", i ? "," : "", t_iter[i]);
        printf("],\"spot_ms\":[");
        for (i = 0; i < repeat; i++) printf("%s%.4f", i ? "," : "", t_spot[i]);
        printf("]}");
    }
#ifdef _WIN32
    {
        PROCESS_MEMORY_COUNTERS pmc;
        FILETIME c, e, k, u;
        ULARGE_INTEGER ku, uu;
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        ku.LowPart = k.dwLowDateTime, ku.HighPart = k.dwHighDateTime;
        uu.LowPart = u.dwLowDateTime, uu.HighPart = u.dwHighDateTime;
        printf(",\"process\":{\"wall_ms\":%.3f,\"cpu_user_ms\":%.3f,\"cpu_kernel_ms\":%.3f,"
               "\"peak_working_set\":%zu,\"peak_private\":%zu,\"startup_working_set\":%zu,"
               "\"startup_private\":%zu}",
               (now() - t_start) * 1e3, (double)uu.QuadPart / 1e4, (double)ku.QuadPart / 1e4,
               (size_t)pmc.PeakWorkingSetSize, (size_t)pmc.PeakPagefileUsage, (size_t)pmc0.WorkingSetSize,
               (size_t)pmc0.PagefileUsage);
    }
#endif
    printf("}\n");
    return 0;
}
