/* perception.c — OCR-based screenshot analysis using Tesseract C API.
 *
 * Multi-pass strip decomposition: the image is sliced into overlapping
 * horizontal strips (80px primary, 40px secondary, 50% overlap) and
 * OCR'd with PSM_SPARSE_TEXT at 2x upscale (OCR_SCALE=2).  Each strip
 * height is processed twice: once on raw RGB and once on contrast-
 * normalized grayscale (pixContrastNorm).  The raw pass preserves
 * color information for highlighted/selected items; the contrast-norm
 * pass reveals low-contrast text that the raw pass misses.  Results
 * from all passes are deduplicated via IoU-based merge.
 *
 * The Tesseract engine handle is initialized once and reused across
 * screenshots, avoiding the ~200ms Python startup + model load overhead
 * that the old subprocess approach incurred on every call. */

#include "perception.h"
#include "cJSON.h"

#ifdef HAVE_TESSERACT

#include <tesseract/capi.h>
#include <leptonica/allheaders.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

/* ── Constants ─────────────────────────────────────────────── */

#define OCR_SCALE       2       /* 2x upscale for small panel text */
#define STRIP_HEIGHT    80      /* px height of horizontal strips */
#define IOU_THRESHOLD   0.4f    /* overlap threshold for merge */
#define MAX_TEXTS       2048    /* max text entries per screenshot */

/* ── Internal text entry ───────────────────────────────────── */

typedef struct {
    char   *text;
    int     bbox[4];    /* x1, y1, x2, y2 */
    int     center[2];  /* cx, cy */
    float   conf;       /* 0.0 - 1.0 */
} ocr_text_t;

typedef struct {
    ocr_text_t *items;
    int         count;
    int         cap;
} ocr_texts_t;


/* ── Static Tesseract handle ──────────────────────────────── */

static TessBaseAPI *g_tess = NULL;
static int g_initialized = 0;

/* ── Helper: texts array management ────────────────────────── */

static void texts_init(ocr_texts_t *t) {
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

static void texts_free(ocr_texts_t *t) {
    for (int i = 0; i < t->count; i++)
        free(t->items[i].text);
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

static int texts_add(ocr_texts_t *t, const char *text,
                     int x1, int y1, int x2, int y2, float conf) {
    if (t->count >= MAX_TEXTS) return -1;
    if (t->count >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        ocr_text_t *ni = realloc(t->items, nc * sizeof(ocr_text_t));
        if (!ni) return -1;
        t->items = ni;
        t->cap = nc;
    }
    ocr_text_t *e = &t->items[t->count];
    e->text = strdup(text);
    if (!e->text) return -1;
    e->bbox[0] = x1; e->bbox[1] = y1;
    e->bbox[2] = x2; e->bbox[3] = y2;
    e->center[0] = (x1 + x2) / 2;
    e->center[1] = (y1 + y2) / 2;
    e->conf = conf;
    t->count++;
    return 0;
}

/* ── Color classification ──────────────────────────────────── */

static const char *classify_color(int r, int g, int b) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int spread = mx - mn;
    if (spread < 30) {
        if (mx > 200) return "white";
        if (mx < 50)  return "black";
        return "gray";
    }
    if (b > r && b > g && b > 140) return "blue";
    if (r > g && r > b && r > 140) {
        if (g > 100) return "orange";
        return "red";
    }
    if (g > r && g > b && g > 140) return "green";
    return "other";
}

/* ── Color sampling ────────────────────────────────────────── */

/* Count occurrences of an RGB value in a pixel array.
 * Returns the most common pixel as r,g,b. */
static void most_common_pixel(l_uint32 *pixels, int n,
                               int *out_r, int *out_g, int *out_b) {
    /* Simple histogram approach: hash RGB to a bucket */
    typedef struct { l_uint32 rgb; int count; } bucket_t;
    bucket_t buckets[256];
    int nbuckets = 0;

    for (int i = 0; i < n && i < 4096; i++) {
        l_uint32 px = pixels[i];
        int found = 0;
        for (int j = 0; j < nbuckets; j++) {
            if (buckets[j].rgb == px) {
                buckets[j].count++;
                found = 1;
                break;
            }
        }
        if (!found && nbuckets < 256) {
            buckets[nbuckets].rgb = px;
            buckets[nbuckets].count = 1;
            nbuckets++;
        }
    }

    int best = 0;
    for (int i = 1; i < nbuckets; i++) {
        if (buckets[i].count > buckets[best].count)
            best = i;
    }
    if (nbuckets > 0) {
        l_uint32 px = buckets[best].rgb;
        /* Leptonica 32-bit pixel: RRGGBB00 or 00RRGGBB depending on endianness.
         * Use extractRGBValues for safety. */
        extractRGBValues(px, out_r, out_g, out_b);
    } else {
        *out_r = *out_g = *out_b = 0;
    }
}

static void sample_colors(PIX *pix, const int *bbox,
                          const char **fg_out, const char **bg_out) {
    *fg_out = NULL;
    *bg_out = NULL;

    int x1 = bbox[0], y1 = bbox[1], x2 = bbox[2], y2 = bbox[3];
    int img_w = pixGetWidth(pix);
    int img_h = pixGetHeight(pix);

    /* Expand bbox by 4px for background sampling */
    int pad = 4;
    int ex1 = x1 - pad; if (ex1 < 0) ex1 = 0;
    int ey1 = y1 - pad; if (ey1 < 0) ey1 = 0;
    int ex2 = x2 + pad; if (ex2 > img_w) ex2 = img_w;
    int ey2 = y2 + pad; if (ey2 > img_h) ey2 = img_h;
    if (ex2 <= ex1 || ey2 <= ey1) return;

    /* Sample background from border pixels */
    l_uint32 bg_pixels[8192];
    int bg_n = 0;

    for (int px = ex1; px < ex2 && bg_n < 8192; px++) {
        l_uint32 val;
        if (pixGetPixel(pix, px, ey1, &val) == 0) bg_pixels[bg_n++] = val;
        if (ey2 - 1 >= 0 && pixGetPixel(pix, px, ey2 - 1, &val) == 0)
            bg_pixels[bg_n++] = val;
    }
    for (int py = ey1; py < ey2 && bg_n < 8192; py++) {
        l_uint32 val;
        if (pixGetPixel(pix, ex1, py, &val) == 0) bg_pixels[bg_n++] = val;
        if (ex2 - 1 >= 0 && pixGetPixel(pix, ex2 - 1, py, &val) == 0)
            bg_pixels[bg_n++] = val;
    }
    if (bg_n == 0) return;

    int bg_r, bg_g, bg_b;
    most_common_pixel(bg_pixels, bg_n, &bg_r, &bg_g, &bg_b);
    *bg_out = classify_color(bg_r, bg_g, bg_b);

    /* Sample foreground from interior pixels that differ from background */
    int ix1 = x1 < 0 ? 0 : x1;
    int iy1 = y1 < 0 ? 0 : y1;
    int ix2 = x2 > img_w ? img_w : x2;
    int iy2 = y2 > img_h ? img_h : y2;

    l_uint32 fg_pixels[4096];
    int fg_n = 0;

    /* Sample up to 4096 interior pixels that differ from bg */
    int step_x = (ix2 - ix1) > 64 ? (ix2 - ix1) / 64 : 1;
    int step_y = (iy2 - iy1) > 64 ? (iy2 - iy1) / 64 : 1;
    for (int py = iy1; py < iy2 && fg_n < 4096; py += step_y) {
        for (int px = ix1; px < ix2 && fg_n < 4096; px += step_x) {
            l_uint32 val;
            if (pixGetPixel(pix, px, py, &val) != 0) continue;
            int r, g, b;
            extractRGBValues(val, &r, &g, &b);
            int diff = abs(r - bg_r) + abs(g - bg_g) + abs(b - bg_b);
            if (diff > 60)
                fg_pixels[fg_n++] = val;
        }
    }
    if (fg_n > 0) {
        int fg_r, fg_g, fg_b;
        most_common_pixel(fg_pixels, fg_n, &fg_r, &fg_g, &fg_b);
        *fg_out = classify_color(fg_r, fg_g, fg_b);
    }
}

/* ── Color tag ─────────────────────────────────────────────── */

/* Returns a static string like "[blue]" or "[red/on-blue]", or "" */
static const char *color_tag(const char *fg, const char *bg) {
    /* Skip boring combos */
    int fg_boring = (!fg || strcmp(fg, "black") == 0 ||
                     strcmp(fg, "gray") == 0);
    int bg_boring = (!bg || strcmp(bg, "white") == 0 ||
                     strcmp(bg, "gray") == 0);
    if (fg_boring && bg_boring) return "";

    int bg_dark_boring = (!bg || strcmp(bg, "black") == 0 ||
                          strcmp(bg, "gray") == 0);
    int fg_light_boring = (!fg || strcmp(fg, "white") == 0 ||
                           strcmp(fg, "gray") == 0);
    if (fg_light_boring && bg_dark_boring) return "";

    static char tag_buf[64];
    int has_fg = (fg && strcmp(fg, "black") != 0 && strcmp(fg, "gray") != 0 &&
                  strcmp(fg, "other") != 0);
    int has_bg = (bg && strcmp(bg, "white") != 0 && strcmp(bg, "gray") != 0 &&
                  strcmp(bg, "other") != 0);

    if (has_fg && has_bg)
        snprintf(tag_buf, sizeof(tag_buf), "[%s/on-%s]", fg, bg);
    else if (has_fg)
        snprintf(tag_buf, sizeof(tag_buf), "[%s]", fg);
    else if (has_bg)
        snprintf(tag_buf, sizeof(tag_buf), "[on-%s]", bg);
    else
        return "";

    return tag_buf;
}

/* ── IoU computation ───────────────────────────────────────── */

static float bbox_iou(const int *a, const int *b) {
    int x1 = a[0] > b[0] ? a[0] : b[0];
    int y1 = a[1] > b[1] ? a[1] : b[1];
    int x2 = a[2] < b[2] ? a[2] : b[2];
    int y2 = a[3] < b[3] ? a[3] : b[3];
    int iw = x2 - x1; if (iw < 0) iw = 0;
    int ih = y2 - y1; if (ih < 0) ih = 0;
    int inter = iw * ih;
    if (inter == 0) return 0.0f;
    int area_a = (a[2] - a[0]) * (a[3] - a[1]);
    int area_b = (b[2] - b[0]) * (b[3] - b[1]);
    int uni = area_a + area_b - inter;
    return uni > 0 ? (float)inter / (float)uni : 0.0f;
}

/* ── Noise filter ──────────────────────────────────────────── */

static int is_noise(const char *text, float conf) {
    int len = (int)strlen(text);
    if (len == 0) return 1;

    /* Single character -- almost never real UI text */
    if (len == 1) return 1;

    /* Count alpha characters */
    int alpha_count = 0;
    for (int i = 0; i < len; i++) {
        if (isalpha((unsigned char)text[i])) alpha_count++;
    }

    /* 2-char fragments: only keep if very high confidence and has alpha
     * (e.g. "en" keyboard layout indicator, "SC" app icon) */
    if (len <= 2 && conf < 0.85f) return 1;
    if (len <= 2 && alpha_count == 0) return 1;

    /* 3-4 char: moderate confidence threshold */
    if (len <= 3 && conf < 0.75f) return 1;
    if (len <= 4 && conf < 0.65f) return 1;

    /* Longer text with very low alpha ratio is likely noise from
     * wallpaper speckles being misread as punctuation/symbols */
    if (len >= 5 && alpha_count * 3 < len) return 1;

    return 0;
}

/* ── Scale coordinates back to original image space ────────── */

static void scale_texts(ocr_texts_t *texts, int factor) {
    if (factor <= 1) return;
    float inv = 1.0f / (float)factor;
    for (int i = 0; i < texts->count; i++) {
        ocr_text_t *t = &texts->items[i];
        for (int j = 0; j < 4; j++)
            t->bbox[j] = (int)(t->bbox[j] * inv);
        t->center[0] = (int)(t->center[0] * inv);
        t->center[1] = (int)(t->center[1] * inv);
    }
}

/* ── Run Tesseract on a Pix with given PSM, emit individual words ────── */
/*
 * Each recognized word is emitted as a separate text entry rather than
 * aggregating into lines.  This prevents menu bar items ("Applications",
 * "Places", "System") from being merged with noise characters that
 * Tesseract places on the same TEXTLINE because they share a horizontal
 * band.  The downstream noise filter (is_noise) handles short/low-conf
 * fragments, and the summary builder groups entries by Y coordinate for
 * readable output.
 */

static void run_tess_pass(PIX *pix, TessPageSegMode psm, ocr_texts_t *out) {
    TessBaseAPISetPageSegMode(g_tess, psm);
    TessBaseAPISetImage2(g_tess, pix);

    if (TessBaseAPIRecognize(g_tess, NULL) != 0) {
        TessBaseAPIClear(g_tess);
        return;
    }

    TessResultIterator *ri = TessBaseAPIGetIterator(g_tess);
    if (!ri) {
        TessBaseAPIClear(g_tess);
        return;
    }

    TessPageIterator *pi = TessResultIteratorGetPageIterator(ri);

    do {
        char *word = TessResultIteratorGetUTF8Text(ri, RIL_WORD);
        if (!word) continue;

        /* Strip leading whitespace */
        char *ws = word;
        while (*ws && isspace((unsigned char)*ws)) ws++;
        if (!*ws) { TessDeleteText(word); continue; }

        float conf = TessResultIteratorConfidence(ri, RIL_WORD);
        if (conf < 40.0f) { TessDeleteText(word); continue; }

        /* Skip single-char noise words early */
        int wlen_raw = (int)strlen(ws);
        if (wlen_raw == 1 && !isalnum((unsigned char)ws[0])) {
            TessDeleteText(word); continue;
        }

        int left, top, right, bottom;
        if (!TessPageIteratorBoundingBox(pi, RIL_WORD,
                                         &left, &top, &right, &bottom)) {
            TessDeleteText(word);
            continue;
        }

        texts_add(out, ws, left, top, right, bottom, conf / 100.0f);
        TessDeleteText(word);
    } while (TessPageIteratorNext(pi, RIL_WORD));

    TessResultIteratorDelete(ri);
    TessBaseAPIClear(g_tess);
}

/* ── Merge two text arrays (keep highest confidence for overlapping bboxes) ── */

static void merge_texts(ocr_texts_t *merged, const ocr_texts_t *b) {
    for (int i = 0; i < b->count; i++) {
        const ocr_text_t *tb = &b->items[i];
        float best_iou = 0.0f;
        int best_idx = -1;
        for (int j = 0; j < merged->count; j++) {
            float iou = bbox_iou(merged->items[j].bbox, tb->bbox);
            if (iou > best_iou) {
                best_iou = iou;
                best_idx = j;
            }
        }
        if (best_iou > IOU_THRESHOLD) {
            /* Overlapping -- keep higher confidence */
            if (tb->conf > merged->items[best_idx].conf) {
                free(merged->items[best_idx].text);
                merged->items[best_idx].text = strdup(tb->text);
                merged->items[best_idx].conf = tb->conf;
                memcpy(merged->items[best_idx].bbox, tb->bbox, sizeof(tb->bbox));
                merged->items[best_idx].center[0] = tb->center[0];
                merged->items[best_idx].center[1] = tb->center[1];
            }
        } else {
            /* New text not in merged */
            texts_add(merged, tb->text, tb->bbox[0], tb->bbox[1],
                      tb->bbox[2], tb->bbox[3], tb->conf);
        }
    }
}

/* ── Offset text coordinates (for strip-based OCR) ─────────── */

static void offset_texts(ocr_texts_t *texts, int dy) {
    if (dy == 0) return;
    for (int i = 0; i < texts->count; i++) {
        ocr_text_t *t = &texts->items[i];
        t->bbox[1] += dy;
        t->bbox[3] += dy;
        t->center[1] += dy;
    }
}

/* ── Strip-pass helper ─────────────────────────────────────── */
/*
 * Process a single set of horizontal strips at the given height
 * and merge results into `out`.  Used by run_ocr() to combine
 * multiple strip heights for better coverage.
 */

static void run_strips(PIX *rgb, int strip_h, int contrast_norm,
                       ocr_texts_t *out) {
    int img_w = pixGetWidth(rgb);
    int img_h = pixGetHeight(rgb);
    int overlap = strip_h / 2;   /* 50% overlap */

    for (int y = 0; y < img_h; y += strip_h - overlap) {
        int y2 = y + strip_h;
        if (y2 > img_h) y2 = img_h;
        if (y2 - y < 10) break;  /* skip tiny remainder */

        /* Crop strip */
        BOX *box = boxCreate(0, y, img_w, y2 - y);
        if (!box) continue;
        PIX *strip = pixClipRectangle(rgb, box, NULL);
        boxDestroy(&box);
        if (!strip) continue;

        PIX *to_scale = strip;

        /* Optional contrast normalization: convert to 8bpp grayscale
         * and apply local contrast normalization.  This makes low-contrast
         * menu text (dark text on slightly-lighter dark background)
         * readable.  pixContrastNorm equalizes local intensity,
         * dramatically improving OCR on JPEG-compressed UI screenshots.
         *
         * Not used for the raw pass, which preserves color information
         * needed for highlighted/selected items (e.g., light text on
         * a subtly different gray background). */
        PIX *enhanced = NULL;
        if (contrast_norm) {
            PIX *gray = pixConvertRGBToGray(strip, 0.0f, 0.0f, 0.0f);
            pixDestroy(&strip);
            if (!gray) continue;
            enhanced = pixContrastNorm(NULL, gray, 100, 100, 55, 1, 1);
            pixDestroy(&gray);
            if (!enhanced) continue;
            to_scale = enhanced;
        }

        /* Upscale using nearest-neighbor (pixExpandReplicate) to preserve
         * sharp text edges.  pixScale (linear interpolation) blurs small
         * menu/panel text enough to make it unrecognizable by Tesseract. */
        PIX *scaled;
        if (OCR_SCALE > 1) {
            scaled = pixExpandReplicate(to_scale, OCR_SCALE);
            if (enhanced) pixDestroy(&enhanced);
            else pixDestroy(&strip);
            if (!scaled) continue;
        } else {
            scaled = to_scale;
        }

        /* OCR this strip */
        ocr_texts_t strip_texts;
        texts_init(&strip_texts);
        run_tess_pass(scaled, PSM_SPARSE_TEXT, &strip_texts);
        pixDestroy(&scaled);

        /* Map coordinates back to original image space */
        scale_texts(&strip_texts, OCR_SCALE);
        offset_texts(&strip_texts, y);

        /* Merge into global results (IoU dedup for overlapping strips) */
        merge_texts(out, &strip_texts);
        texts_free(&strip_texts);
    }
}

/* ── Main OCR pipeline ─────────────────────────────────────── */
/*
 * Multi-pass strip decomposition: the image is sliced into overlapping
 * horizontal strips at two different heights (STRIP_HEIGHT and half)
 * and OCR'd independently.  This ensures that text sitting at a strip
 * boundary in one pass is fully contained in the other.
 *
 * An additional inverted-color pass catches light text on colored
 * backgrounds (e.g., highlighted/selected menu items like white-on-blue
 * "Internet") that the normal pass misses.
 *
 * Results from all passes are deduplicated via IoU-based merge.
 */

static void run_ocr(const char *image_path, ocr_texts_t *out) {
    PIX *orig = pixRead(image_path);
    if (!orig) return;

    /* Ensure 32-bit RGB */
    PIX *rgb = NULL;
    if (pixGetDepth(orig) != 32) {
        rgb = pixConvertTo32(orig);
        pixDestroy(&orig);
        if (!rgb) return;
    } else {
        rgb = orig;
    }

    texts_init(out);

    /* Pass 1: raw RGB strips at primary height (80px).
     * Preserves color information needed for highlighted/selected
     * items and text with subtle color differences. */
    run_strips(rgb, STRIP_HEIGHT, 0, out);

    /* Pass 2: raw RGB at half-height (40px) to catch text at
     * pass-1 strip boundaries. */
    if (STRIP_HEIGHT > 40)
        run_strips(rgb, STRIP_HEIGHT / 2, 0, out);

    /* Pass 3: contrast-normalized strips at primary height.
     * pixContrastNorm equalizes local intensity, dramatically
     * improving OCR on low-contrast text (dark-on-gray menus,
     * JPEG-compressed UI elements). */
    run_strips(rgb, STRIP_HEIGHT, 1, out);

    /* Pass 4: contrast-normalized at half-height. */
    if (STRIP_HEIGHT > 40)
        run_strips(rgb, STRIP_HEIGHT / 2, 1, out);

    pixDestroy(&rgb);

    /* Filter noise */
    int write_idx = 0;
    for (int i = 0; i < out->count; i++) {
        if (!is_noise(out->items[i].text, out->items[i].conf)) {
            if (write_idx != i)
                out->items[write_idx] = out->items[i];
            write_idx++;
        } else {
            free(out->items[i].text);
        }
    }
    out->count = write_idx;
}

/* ── Sort comparator: top-to-bottom, left-to-right ─────────── */

static int text_cmp(const void *a, const void *b) {
    const ocr_text_t *ta = (const ocr_text_t *)a;
    const ocr_text_t *tb = (const ocr_text_t *)b;
    if (ta->center[1] != tb->center[1])
        return ta->center[1] - tb->center[1];
    return ta->center[0] - tb->center[0];
}

/* ── Public API ────────────────────────────────────────────── */

int perception_init(void) {
    if (g_initialized) return 0;

    /* Suppress all Leptonica messages (info, warnings, etc.) */
    setMsgSeverity(L_SEVERITY_NONE);

    g_tess = TessBaseAPICreate();
    if (!g_tess) return -1;

    /* Redirect stderr during Init3 — Tesseract prints debug messages
     * to stderr during initialization before we can set debug_file. */
    int saved_stderr = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    int rc = TessBaseAPIInit3(g_tess, NULL, "eng");

    /* Restore stderr */
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }

    if (rc != 0) {
        TessBaseAPIDelete(g_tess);
        g_tess = NULL;
        return -1;
    }

    /* Suppress Tesseract debug output during recognition */
    TessBaseAPISetVariable(g_tess, "debug_file", "/dev/null");
    TessBaseAPISetVariable(g_tess, "classify_debug_level", "0");

    g_initialized = 1;
    return 0;
}

cJSON *perception_analyze(const char *screenshot_path) {
    if (!g_initialized) {
        if (perception_init() != 0)
            return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    cJSON_AddStringToObject(result, "screenshot_path", screenshot_path);
    cJSON *errors_arr = cJSON_AddArrayToObject(result, "errors");

    /* Run OCR pipeline */
    ocr_texts_t texts;
    texts_init(&texts);
    run_ocr(screenshot_path, &texts);

    /* Load original image for color sampling */
    PIX *orig_pix = pixRead(screenshot_path);
    int img_w = 0, img_h = 0;
    if (orig_pix) {
        if (pixGetDepth(orig_pix) != 32) {
            PIX *tmp = pixConvertTo32(orig_pix);
            pixDestroy(&orig_pix);
            orig_pix = tmp;
        }
        if (orig_pix) {
            img_w = pixGetWidth(orig_pix);
            img_h = pixGetHeight(orig_pix);
        }
    }

    /* Build texts JSON array with color sampling */
    cJSON *texts_arr = cJSON_AddArrayToObject(result, "texts");

    /* Sort for summary output */
    if (texts.count > 0)
        qsort(texts.items, texts.count, sizeof(ocr_text_t), text_cmp);

    for (int i = 0; i < texts.count; i++) {
        ocr_text_t *t = &texts.items[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "text", t->text);

        cJSON *bbox = cJSON_CreateIntArray(t->bbox, 4);
        cJSON_AddItemToObject(entry, "bbox", bbox);

        cJSON *center = cJSON_CreateIntArray(t->center, 2);
        cJSON_AddItemToObject(entry, "center", center);

        cJSON_AddNumberToObject(entry, "conf",
            (double)((int)(t->conf * 100.0f + 0.5f)) / 100.0);

        /* Color sampling */
        const char *fg = NULL, *bg = NULL;
        if (orig_pix)
            sample_colors(orig_pix, t->bbox, &fg, &bg);

        cJSON_AddStringToObject(entry, "fg", fg ? fg : "");
        cJSON_AddStringToObject(entry, "bg", bg ? bg : "");
        const char *ctag = color_tag(fg, bg);
        cJSON_AddStringToObject(entry, "color_tag", ctag);

        cJSON_AddItemToArray(texts_arr, entry);
    }

    /* Build summary */
    /* Calculate needed buffer size */
    size_t sum_cap = 256 + texts.count * 128;
    char *summary = malloc(sum_cap);
    if (summary) {
        int off = 0;
        off += snprintf(summary + off, sum_cap - off,
                       "Screenshot: %s ( %dx%d, %d texts)",
                       screenshot_path, img_w, img_h, texts.count);

        if (texts.count > 0) {
            off += snprintf(summary + off, sum_cap - off,
                           "\n\nScreen text (%d lines):", texts.count);
            int prev_y = -999;
            for (int i = 0; i < texts.count; i++) {
                ocr_text_t *t = &texts.items[i];
                int cy = t->center[1];
                if (prev_y >= 0 && cy - prev_y > 30)
                    off += snprintf(summary + off, sum_cap - off, "\n");
                prev_y = cy;

                const char *fg = NULL, *bg = NULL;
                if (orig_pix)
                    sample_colors(orig_pix, t->bbox, &fg, &bg);
                const char *ctag = color_tag(fg, bg);

                if (ctag[0])
                    off += snprintf(summary + off, sum_cap - off,
                                   "\n  \"%s\" @%d,%d %s",
                                   t->text, t->center[0], t->center[1], ctag);
                else
                    off += snprintf(summary + off, sum_cap - off,
                                   "\n  \"%s\" @%d,%d",
                                   t->text, t->center[0], t->center[1]);
                if ((size_t)off >= sum_cap - 2) break;
            }
        }
        cJSON_AddStringToObject(result, "summary", summary);
        free(summary);
    } else {
        cJSON_AddStringToObject(result, "summary", "");
    }

    cJSON_AddNumberToObject(result, "widget_count", 0);
    cJSON_AddNumberToObject(result, "text_count", texts.count);

    if (orig_pix) pixDestroy(&orig_pix);
    texts_free(&texts);

    (void)errors_arr;  /* errors array stays empty unless we add to it */

    return result;
}

void perception_cleanup(void) {
    if (g_tess) {
        TessBaseAPIEnd(g_tess);
        TessBaseAPIDelete(g_tess);
        g_tess = NULL;
    }
    g_initialized = 0;
}

#else /* !HAVE_TESSERACT */

/* Stub implementations when Tesseract is not available */

int perception_init(void) {
    return -1;
}

cJSON *perception_analyze(const char *screenshot_path) {
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    cJSON_AddStringToObject(result, "screenshot_path", screenshot_path);
    cJSON *arr = cJSON_AddArrayToObject(result, "texts");
    (void)arr;
    cJSON_AddStringToObject(result, "summary",
                            "OCR unavailable (compiled without Tesseract)");
    cJSON_AddNumberToObject(result, "widget_count", 0);
    cJSON_AddNumberToObject(result, "text_count", 0);
    cJSON *errors = cJSON_AddArrayToObject(result, "errors");
    cJSON_AddItemToArray(errors,
        cJSON_CreateString("Tesseract not available -- install tesseract-devel and rebuild"));
    return result;
}

void perception_cleanup(void) {}

#endif /* HAVE_TESSERACT */

/* ── Standalone test binary ────────────────────────────────── */

#ifdef __PERCEPTION_TEST

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image-file>\n", argv[0]);
        return 1;
    }

    if (perception_init() != 0) {
        fprintf(stderr, "error: failed to initialize perception engine\n");
        return 1;
    }

    cJSON *result = perception_analyze(argv[1]);
    if (!result) {
        fprintf(stderr, "error: perception_analyze failed for '%s'\n", argv[1]);
        perception_cleanup();
        return 1;
    }

    char *json_str = cJSON_Print(result);
    if (json_str) {
        puts(json_str);
        free(json_str);
    }

    /* Also print the summary to stderr for quick inspection */
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(result, "summary");
    if (cJSON_IsString(summary) && summary->valuestring) {
        fprintf(stderr, "\n%s\n", summary->valuestring);
    }

    cJSON_Delete(result);
    perception_cleanup();
    return 0;
}

#endif /* __PERCEPTION_TEST */
