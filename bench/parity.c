/* parity - hash every output of the recognizer and the line finder over a set of frames, to compare builds bit for bit.
 *
 *   parity --models DIR [--threads N] [--tile F] [--head F] FRAME.ppm...
 *
 * Recognizer, per tier (S, N, L): random line crops of every frame (fixed seed, both polarities), each read twice:
 *   pass A: head gate OFF, full head, full logits + best per frame;
 *   pass B: Rubi's options (gate ON where the blob has one, two-level head where it has one), top-8, blank logits,
 *           conf, gated / escaped counts.
 * Line finder: every frame grey and RGBA, at the default and the forced 1x scale: every map row, box and block.
 * Each prints one FNV-1a 64 hash; two builds that print the same hashes produced identical bytes.
 */
#include "vocr.h"
#include "vocr_textspot6.h"
#ifdef _WIN32
#include "pool.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CROPS_PER_FRAME 20 /* ~ 2 s per frame for the scalar build */
#define MAX_WIDTH 1024
#define TOPK 8
#define MAX_FRAMES 256

static const char *BLOBS[3] = {"VOCR6.2_S_w8.vocr", "VOCR6.2_N_w8g2.vocr", "VOCR6.2_L_w8_2l.vocr"};

static uint64_t h;
static void mix(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
}

static uint32_t rng;
static unsigned rnd(unsigned n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

static void *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    void *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) exit(2);
    fclose(f);
    return b;
}

/* binary PPM -> RGB; PGM -> grey copied into all three channels */
static unsigned char *read_rgb(const char *path, unsigned *w, unsigned *hgt)
{
    FILE *f = fopen(path, "rb");
    unsigned mx, i;
    char kind = 0;
    unsigned char *rgb;
    if (!f || fscanf(f, "P%c %u %u %u", &kind, w, hgt, &mx) != 4 || fgetc(f) == EOF || (kind != '5' && kind != '6')) {
        fprintf(stderr, "%s: not a binary PGM/PPM\n", path);
        exit(2);
    }
    rgb = malloc((size_t)*w * *hgt * 3);
    if (fread(rgb, kind == '6' ? 3 : 1, (size_t)*w * *hgt, f) != (size_t)*w * *hgt) exit(2);
    fclose(f);
    if (kind == '5')
        for (i = *w * *hgt; i-- > 0;) rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = rgb[i];
    return rgb;
}

static unsigned char *grey(const unsigned char *rgb, unsigned n)
{
    unsigned char *g = malloc(n);
    unsigned i;
    for (i = 0; i < n; i++) g[i] = (unsigned char)((77 * rgb[3 * i] + 150 * rgb[3 * i + 1] + 29 * rgb[3 * i + 2]) >> 8);
    return g;
}

static void recognizer(const char *models, const char **frames, unsigned nf, unsigned tile, unsigned head)
{
    unsigned t, i, c;
    for (t = 0; t < 3; t++) {
        char path[1024], text[4096];
        size_t bn, an[2];
        void *blob, *arena[2];
        vocr_model m;
        vocr_ctx ctx[2];
        vocr_opts o[2];
        unsigned ncls, cap = MAX_WIDTH / 4 + 2, crops = 0, frames_total = 0;
        float *logits, *topv, *blank;
        uint16_t *best, *topc;
        snprintf(path, sizeof path, "%s/%s", models, BLOBS[t]);
        blob = read_file(path, &bn);
        if (!blob || vocr_load(&m, blob, bn)) {
            fprintf(stderr, "%s: cannot load\n", path);
            exit(2);
        }
        ncls = m.n_classes;
        memset(o, 0, sizeof o);
        for (i = 0; i < 2; i++) {
            o[i].mode = VOCR_MODE_W8I;
            o[i].max_width = MAX_WIDTH;
            o[i].tile_frames = tile;
            o[i].head_frames = head;
        }
        o[0].head_gate = VOCR_HEAD_GATE_OFF;
        o[1].head_gate = vocr_has_gate(&m) ? VOCR_HEAD_GATE_ON : VOCR_HEAD_GATE_OFF;
        o[1].two_level = (unsigned)vocr_has_two_level(&m);
        for (i = 0; i < 2; i++) {
            an[i] = vocr_arena_bytes(&m, &o[i]);
            arena[i] = malloc(an[i]);
            if (vocr_init(&ctx[i], &m, &o[i], arena[i], an[i])) exit(2);
        }
        logits = malloc((size_t)cap * ncls * sizeof(float));
        best = malloc(cap * sizeof(uint16_t));
        topv = malloc(cap * TOPK * sizeof(float));
        topc = malloc(cap * TOPK * sizeof(uint16_t));
        blank = malloc(cap * sizeof(float));
        h = 1469598103934665603ull;
        rng = 12345u;
        for (i = 0; i < nf; i++) {
            unsigned W, H;
            unsigned char *rgb = read_rgb(frames[i], &W, &H), *g = grey(rgb, W * H);
            for (c = 0; c < CROPS_PER_FRAME; c++) {
                vocr_image im;
                vocr_extra ex;
                unsigned ch = 12 + rnd(70), cw = 8 + rnd(W - 8), x, y;
                if (ch > H) ch = H;
                x = rnd(W - cw + 1);
                y = rnd(H - ch + 1);
                im.pixels = g + (size_t)y * W + x;
                im.width = cw;
                im.height = ch;
                im.stride = W;
                im.format = VOCR_GREY8 | (c & 1 ? VOCR_INVERT : 0);
                memset(&ex, 0, sizeof ex);
                ex.logits = logits;
                ex.best = best;
                ex.cap_frames = cap;
                if (vocr_run(&ctx[0], &im, text, sizeof text, &ex)) exit(3);
                mix(text, strlen(text) + 1);
                mix(&ex.frames, sizeof ex.frames);
                mix(logits, (size_t)ex.frames * ncls * sizeof(float));
                mix(best, ex.frames * sizeof(uint16_t));
                frames_total += ex.frames;
                memset(&ex, 0, sizeof ex);
                ex.cap_frames = cap;
                ex.topk = TOPK;
                ex.topk_logit = topv;
                ex.topk_class = topc;
                ex.blank_logit = blank;
                if (vocr_run(&ctx[1], &im, text, sizeof text, &ex)) exit(3);
                mix(text, strlen(text) + 1);
                mix(topv, (size_t)ex.frames * TOPK * sizeof(float));
                mix(topc, (size_t)ex.frames * TOPK * sizeof(uint16_t));
                mix(blank, ex.frames * sizeof(float));
                mix(&ex.conf_min, sizeof ex.conf_min);
                mix(&ex.conf_mean, sizeof ex.conf_mean);
                mix(&ex.gated, sizeof ex.gated);
                mix(&ex.escaped, sizeof ex.escaped);
                crops++;
            }
            free(g), free(rgb);
        }
        printf("recognizer %c crops=%u frames=%u hash=%016llx\n", "SNL"[t], crops, frames_total, (unsigned long long)h);
        free(logits), free(best), free(topv), free(topc), free(blank), free(arena[0]), free(arena[1]), free(blob);
    }
}

static void on_row(void *u, unsigned i, const signed char *maps, unsigned w)
{
    mix(&i, sizeof i);
    mix(maps, (size_t)w * *(unsigned *)u);
}

static void finder(const char *models, const char **frames, unsigned nf)
{
    char path[1024];
    size_t bn;
    void *blob;
    vocr_textspot6 m;
    unsigned planes, i, k, v, boxes = 0;
    snprintf(path, sizeof path, "%s/%s", models, "textspot8_t64n8c16_w8.vtsp");
    blob = read_file(path, &bn);
    if (!blob || vocr_textspot6_load(&m, blob, bn)) {
        fprintf(stderr, "%s: cannot load\n", path);
        exit(2);
    }
    planes = 4u + m.tags;
    h = 1469598103934665603ull;
    for (i = 0; i < nf; i++) {
        unsigned W, H;
        unsigned char *rgb = read_rgb(frames[i], &W, &H), *g = grey(rgb, W * H), *rgba;
        if (W > VOCR_TEXTSPOT_MAX_SIDE || H > VOCR_TEXTSPOT_MAX_SIDE) { /* the finder's limit: skipped */
            free(g), free(rgb);
            continue;
        }
        rgba = malloc((size_t)W * H * 4);
        for (k = 0; k < W * H; k++)
            rgba[4 * k] = rgb[3 * k], rgba[4 * k + 1] = rgb[3 * k + 1], rgba[4 * k + 2] = rgb[3 * k + 2], rgba[4 * k + 3] = 255;
        for (v = 0; v < 4; v++) { /* grey / RGBA x default / forced 1x scale */
            VocrLinesImage im;
            VocrTextspot6Set set;
            unsigned fl = v & 2u ? VOCR_TEXTSPOT6_SCALE1 : 0u;
            size_t an;
            void *arena;
            im.pixels = v & 1u ? rgba : g, im.width = W, im.height = H;
            im.stride = (size_t)W * (v & 1u ? 4u : 1u), im.bpp = v & 1u ? VOCR_LINES_RGBA : VOCR_LINES_GREY;
            an = vocr_textspot6_frame_arena_bytes_flags(&m, W, H, fl);
            arena = malloc(an);
            if (vocr_textspot6_maps(&m, &im, fl, arena, an, on_row, &planes)) exit(3);
            if (vocr_textspot6_find(&m, &im, fl, arena, an, &set)) exit(3);
            mix(&set.count, sizeof set.count);
            mix(set.boxes, set.count * sizeof set.boxes[0]);
            mix(&set.block_count, sizeof set.block_count);
            mix(set.blocks, set.block_count * sizeof set.blocks[0]);
            boxes += set.count;
            free(arena);
        }
        free(rgba), free(g), free(rgb);
    }
    printf("finder boxes=%u hash=%016llx\n", boxes, (unsigned long long)h);
    free(blob);
}

int main(int argc, char **argv)
{
    const char *models = "models";
    const char *frames[MAX_FRAMES];
    unsigned nf = 0, i, tile = 0, head = 0;
    for (i = 1; i < (unsigned)argc; i++) {
        if (!strcmp(argv[i], "--models") && i + 1 < (unsigned)argc) models = argv[++i];
#ifdef _WIN32
        else if (!strcmp(argv[i], "--threads") && i + 1 < (unsigned)argc) { /* products and finder rows on N threads */
            pool_start((unsigned)atoi(argv[++i]));
            vocr_set_parallel(pool_run, NULL, pool_ways());
            vocr_textspot6_set_parallel(pool_run, NULL, pool_ways());
        }
#endif
        else if (!strcmp(argv[i], "--tile") && i + 1 < (unsigned)argc) tile = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--head") && i + 1 < (unsigned)argc) head = (unsigned)atoi(argv[++i]);
        else if (nf < MAX_FRAMES) frames[nf++] = argv[i];
    }
    if (!nf) {
        fprintf(stderr, "usage: parity --models DIR [--threads N] [--tile F] [--head F] FRAME.ppm...\n");
        return 2;
    }
    recognizer(models, frames, nf, tile, head);
    finder(models, frames, nf);
    return 0;
}
