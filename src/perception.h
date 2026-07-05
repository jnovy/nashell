/* perception.h — OCR-based screenshot analysis using Tesseract C API.
 *
 * Replaces the Python perception.py script with a native C implementation.
 * Uses Tesseract + Leptonica for 2x upscale dual-pass OCR (normal + inverted)
 * with IoU-based merge and noise filtering.
 *
 * The Tesseract engine is initialized once and reused across screenshots
 * to avoid repeated ~200ms startup overhead. */
#ifndef PERCEPTION_H
#define PERCEPTION_H

#include "cJSON.h"

/* Initialize the perception engine (Tesseract + Leptonica).
 * Call once at startup or lazily on first use.
 * Returns 0 on success, -1 on failure. */
int perception_init(void);

/* Analyze a screenshot file and return structured JSON results.
 * The returned cJSON object has the same schema as the old perception.py:
 *   {
 *     "screenshot_path": "...",
 *     "texts":   [ {text, bbox, center, conf, fg, bg, color_tag}, ... ],
 *     "summary": "human-readable description",
 *     "widget_count": 0,
 *     "text_count": N,
 *     "errors": []
 *   }
 * Caller owns the returned cJSON and must cJSON_Delete() it.
 * Returns NULL on fatal error. */
cJSON *perception_analyze(const char *screenshot_path);

/* Shut down the perception engine and free resources.
 * Safe to call even if perception_init() was never called. */
void perception_cleanup(void);

#endif /* PERCEPTION_H */
