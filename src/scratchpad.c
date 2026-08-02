#include "scratchpad.h"
#include "str.h"
#include "nash_limits.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

void scratchpad_init(scratchpad_t *sp) {
    memset(sp, 0, sizeof(*sp));
    pthread_mutex_init(&sp->mtx, NULL);  /* FIX CRIT2: thread-safe scratchpad */
    sp->cap = SCRATCHPAD_INIT_CAP;
    sp->sections = calloc((size_t)sp->cap, sizeof(scratchpad_section_t));
    if (!sp->sections) sp->cap = 0;
}

/* Clear all scratchpad data but preserve the mutex.
 * Used by scratchpad_parse() which needs to reset contents
 * without destroying the mutex it will immediately re-use. */
static void scratchpad_reset(scratchpad_t *sp) {
    for (int i = 0; i < sp->count; i++) {
        free(sp->sections[i].name);
        free(sp->sections[i].content);
    }
    free(sp->sections);
    sp->sections = NULL;
    sp->count = 0;
    sp->cap = 0;
    for (int i = 0; i < sp->n_cleared; i++)
        free(sp->cleared_names[i]);
    free(sp->cleared_names);
    sp->cleared_names = NULL;
    sp->n_cleared = 0;
    sp->cleared_cap = 0;
}

void scratchpad_free(scratchpad_t *sp) {
    scratchpad_reset(sp);
    pthread_mutex_destroy(&sp->mtx);  /* FIX CRIT2 */
}

void scratchpad_move(scratchpad_t *dst, scratchpad_t *src) {
    /* FIX BUG#4: use scratchpad_reset() to preserve dst's mutex.
     * FIX BUG#8: Don't bitwise-copy pthread_mutex_t (undefined behavior).
     * Copy data fields individually, keep dst's existing mutex. */
    scratchpad_reset(dst);
    dst->sections = src->sections;
    dst->count = src->count;
    dst->cap = src->cap;
    /* dst->mtx is preserved (not destroyed/re-initialized) */
    /* Destroy src's mutex properly before zeroing */
    pthread_mutex_destroy(&src->mtx);
    memset(src, 0, sizeof(*src));
}

/* Ensure capacity for at least one more section. */
static int scratchpad_grow(scratchpad_t *sp) {
    if (sp->count < sp->cap) return 0;
    int new_cap = sp->cap ? sp->cap * 2 : SCRATCHPAD_INIT_CAP;
    if (safe_realloc((void **)&sp->sections,
                     (size_t)new_cap * sizeof(scratchpad_section_t))) return -1;
    memset(sp->sections + sp->cap, 0, (size_t)(new_cap - sp->cap) * sizeof(scratchpad_section_t));
    sp->cap = new_cap;
    return 0;
}

int scratchpad_find(scratchpad_t *sp, const char *name) {
    for (int i = 0; i < sp->count; i++) {
        if (strcmp(sp->sections[i].name, name) == 0)
            return i;
    }
    return -1;
}

int scratchpad_write(scratchpad_t *sp, const char *name, const char *content, int priority) {
    if (priority < 1) priority = 1;
    if (priority > 9) priority = 9;

    pthread_mutex_lock(&sp->mtx);  /* FIX CRIT2 */
    int idx = scratchpad_find(sp, name);
    if (idx >= 0) {
        /* Overwrite existing section */
        free(sp->sections[idx].content);
        sp->sections[idx].content = strdup(content);
        sp->sections[idx].priority = priority;
        sp->sections[idx].dirty = 1;
        pthread_mutex_unlock(&sp->mtx);
        return 0;
    }
    if (scratchpad_grow(sp) < 0) {
        pthread_mutex_unlock(&sp->mtx);
        return -1;  /* allocation failed */
    }

    sp->sections[sp->count].name = strdup(name);
    sp->sections[sp->count].content = strdup(content);
    sp->sections[sp->count].priority = priority;
    sp->sections[sp->count].dirty = 1;
    sp->count++;
    pthread_mutex_unlock(&sp->mtx);
    return 0;
}

int scratchpad_append(scratchpad_t *sp, const char *name, const char *content, int priority) {
    pthread_mutex_lock(&sp->mtx);  /* FIX CRIT2 */
    int idx = scratchpad_find(sp, name);
    if (idx >= 0) {
        /* Append to existing */
        size_t old_len = strlen(sp->sections[idx].content);
        size_t add_len = strlen(content);
        char *combined = malloc(old_len + add_len + 2);  /* +newline+nul */
        if (!combined) { pthread_mutex_unlock(&sp->mtx); return -1; }
        memcpy(combined, sp->sections[idx].content, old_len);
        combined[old_len] = '\n';
        memcpy(combined + old_len + 1, content, add_len);
        combined[old_len + 1 + add_len] = '\0';
        free(sp->sections[idx].content);
        sp->sections[idx].content = combined;
        sp->sections[idx].dirty = 1;
        pthread_mutex_unlock(&sp->mtx);
        return 0;
    }
    /* Create new section inline while still holding the lock to avoid
     * TOCTOU race (another thread could create the same section between
     * our unlock and scratchpad_write's re-lock). */
    if (scratchpad_grow(sp) < 0) {
        pthread_mutex_unlock(&sp->mtx);
        return -1;
    }
    sp->sections[sp->count].name = strdup(name);
    sp->sections[sp->count].content = strdup(content);
    sp->sections[sp->count].priority = priority;
    sp->sections[sp->count].dirty = 1;
    sp->count++;
    pthread_mutex_unlock(&sp->mtx);
    return 0;
}

int scratchpad_clear(scratchpad_t *sp, const char *name) {
    pthread_mutex_lock(&sp->mtx);  /* FIX CRIT2 */
    int idx = scratchpad_find(sp, name);
    if (idx < 0) { pthread_mutex_unlock(&sp->mtx); return -1; }

    /* Track cleared name for JSONL op:clear on next save */
    if (sp->n_cleared >= sp->cleared_cap) {
        int new_cap = sp->cleared_cap ? sp->cleared_cap * 2 : 8;
        if (!safe_realloc((void **)&sp->cleared_names, (size_t)new_cap * sizeof(char *))) {
            sp->cleared_cap = new_cap;
        }
    }
    if (sp->n_cleared < sp->cleared_cap) {
        sp->cleared_names[sp->n_cleared++] = strdup(sp->sections[idx].name);
    }

    free(sp->sections[idx].name);
    free(sp->sections[idx].content);

    /* Shift remaining sections down */
    for (int i = idx; i < sp->count - 1; i++)
        sp->sections[i] = sp->sections[i + 1];
    sp->count--;
    pthread_mutex_unlock(&sp->mtx);
    return 0;
}

/* Compare sections by priority for qsort (lower priority number = first).
 * E3 FIX: Use safe comparison macro instead of subtraction. While safe
 * in practice (priorities 1-9), subtraction-based comparison is a
 * maintenance hazard — inconsistent with SAFE_CMP used elsewhere. */
static int section_cmp(const void *a, const void *b) {
    const scratchpad_section_t *sa = a;
    const scratchpad_section_t *sb = b;
    return (sa->priority > sb->priority) - (sa->priority < sb->priority);
}

char *scratchpad_serialize(scratchpad_t *sp) {
    pthread_mutex_lock(&sp->mtx);  /* FIX CRIT2 */
    if (sp->count == 0) { pthread_mutex_unlock(&sp->mtx); return NULL; }

    /* Deep-copy sections so we can safely unlock before serializing.
     * FIX BUG#1: shallow memcpy left dangling pointers after unlock. */
    int n = sp->count;
    scratchpad_section_t *sorted = malloc((size_t)n * sizeof(scratchpad_section_t));
    if (!sorted) { pthread_mutex_unlock(&sp->mtx); return NULL; }
    for (int i = 0; i < n; i++) {
        sorted[i].name = strdup(sp->sections[i].name);
        sorted[i].content = strdup(sp->sections[i].content);
        sorted[i].priority = sp->sections[i].priority;
    }
    pthread_mutex_unlock(&sp->mtx);  /* safe — working on deep copies */

    qsort(sorted, (size_t)n, sizeof(scratchpad_section_t), section_cmp);

    str_t out = str_new(2048);
    for (int i = 0; i < n; i++) {
        str_appendf(&out, "## %s\n%s\n\n", sorted[i].name, sorted[i].content);
    }
    for (int i = 0; i < n; i++) {
        free(sorted[i].name);
        free(sorted[i].content);
    }
    free(sorted);
    return str_steal(&out);
}

char *scratchpad_serialize_budget(scratchpad_t *sp, size_t max_chars) {
    pthread_mutex_lock(&sp->mtx);  /* FIX CRIT2 */
    if (sp->count == 0) { pthread_mutex_unlock(&sp->mtx); return NULL; }

    /* Deep-copy sections so we can safely unlock before serializing.
     * FIX BUG#1: shallow memcpy left dangling pointers after unlock. */
    int n = sp->count;
    scratchpad_section_t *sorted = malloc((size_t)n * sizeof(scratchpad_section_t));
    if (!sorted) { pthread_mutex_unlock(&sp->mtx); return NULL; }
    for (int i = 0; i < n; i++) {
        sorted[i].name = strdup(sp->sections[i].name);
        sorted[i].content = strdup(sp->sections[i].content);
        sorted[i].priority = sp->sections[i].priority;
    }
    pthread_mutex_unlock(&sp->mtx);  /* safe — working on deep copies */

    qsort(sorted, (size_t)n, sizeof(scratchpad_section_t), section_cmp);

    str_t out = str_new(max_chars > 4096 ? 4096 : max_chars);
    for (int i = 0; i < n; i++) {
        /* Calculate how much space this section needs */
        size_t header_len = strlen(sorted[i].name) + 6;  /* "## " + name + "\n" + trailing "\n\n" */
        size_t content_len = strlen(sorted[i].content);
        size_t section_total = header_len + content_len;
        size_t remaining = (max_chars > out.len) ? (max_chars - out.len) : 0;

        if (remaining < header_len + 20) {
            /* Not enough room even for a header + minimal content — drop this and all lower-priority */
            break;
        }

        str_appendf(&out, "## %s\n", sorted[i].name);

        if (section_total <= remaining) {
            /* Fits fully */
            str_append_cstr(&out, sorted[i].content);
        } else {
            /* Truncate content to fit budget (defensive: guard against underflow).
             * Clamp to UTF-8 boundary to avoid splitting multi-byte chars. */
            size_t avail = (remaining > header_len + 12) ? (remaining - header_len - 12) : 0;
            avail = utf8_clamp(sorted[i].content, avail);
            if (avail > 0) str_append(&out, sorted[i].content, avail);
            str_append_cstr(&out, "\n[truncated]");
        }
        str_append_cstr(&out, "\n\n");
    }

    for (int i = 0; i < n; i++) {
        free(sorted[i].name);
        free(sorted[i].content);
    }
    free(sorted);
    if (out.len == 0) {
        str_free(&out);
        return NULL;
    }
    return str_steal(&out);
}

/* FIX C3: Lightweight size computation — avoids allocating/freeing a full
 * serialized string just to measure strlen(). */
size_t scratchpad_total_size(scratchpad_t *sp) {
    pthread_mutex_lock(&sp->mtx);
    if (sp->count == 0) { pthread_mutex_unlock(&sp->mtx); return 0; }
    size_t total = 0;
    for (int i = 0; i < sp->count; i++) {
        total += strlen(sp->sections[i].name) + 6;  /* "## " + name + "\n" + "\n\n" */
        total += strlen(sp->sections[i].content);
    }
    pthread_mutex_unlock(&sp->mtx);
    return total;
}

/* ── Legacy scratchpad.md loader (fallback for pre-v4 sessions) ──── */
static int scratchpad_load_legacy(scratchpad_t *sp, const char *session_dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/scratchpad.md", session_dir);

    char *buf = slurp_file(path, NULL);
    if (!buf) return -1;
    if (buf[0] == '\0') { free(buf); return 0; }

    /* Parse sections from the file format:
     * <!-- priority:N -->
     * ## section_name
     * content...
     */
    char *pos = buf;
    while (pos && *pos) {
        int priority = 5;  /* default */

        /* Look for priority comment */
        if (strncmp(pos, "<!-- priority:", 14) == 0) {
            priority = atoi(pos + 14);
            if (priority < 1) priority = 1;
            if (priority > 9) priority = 9;
            pos = strchr(pos, '\n');
            if (pos) pos++;
        }

        /* Look for ## header */
        if (!pos || strncmp(pos, "## ", 3) != 0) {
            /* Skip to next line */
            pos = strchr(pos, '\n');
            if (pos) pos++;
            continue;
        }

        /* Extract section name */
        const char *name_start = pos + 3;
        const char *name_end = strchr(name_start, '\n');
        if (!name_end) break;

        char name[256];
        size_t nlen = (size_t)(name_end - name_start);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, name_start, nlen);
        name[nlen] = '\0';

        /* Extract content until next "<!-- priority:" or "## " or EOF */
        const char *content_start = name_end + 1;
        const char *content_end = NULL;

        /* Scan forward for next section marker */
        const char *scan = content_start;
        while (*scan) {
            if (strncmp(scan, "<!-- priority:", 14) == 0 ||
                (strncmp(scan, "## ", 3) == 0 && (scan == buf || *(scan-1) == '\n'))) {
                content_end = scan;
                break;
            }
            scan++;
        }
        if (!content_end) content_end = buf + strlen(buf);

        /* Trim trailing whitespace from content */
        while (content_end > content_start &&
               (*(content_end-1) == '\n' || *(content_end-1) == ' '))
            content_end--;

        size_t clen = (size_t)(content_end - content_start);
        char *content = malloc(clen + 1);
        if (content) {
            memcpy(content, content_start, clen);
            content[clen] = '\0';
            scratchpad_write(sp, name, content, priority);
            free(content);
        }

        pos = (char *)content_end;
        /* Skip whitespace between sections */
        while (*pos == '\n' || *pos == ' ') pos++;
    }

    /* If file was non-empty but no sections were parsed, treat the entire
     * content as a single "default" section (legacy plain-text scratchpad). */
    if (sp->count == 0) {
        char *raw = slurp_file(path, NULL);
        if (raw && raw[0]) {
            scratchpad_write(sp, "default", raw, 5);
        }
        free(raw);
    }

    free(buf);
    return 0;
}

/* ── JSONL scratchpad persistence (v4) ──────────────────────── */

/* DEDUP1: Shared helper — write one scratchpad section as a JSONL line.
 * Used by both scratchpad_save (append dirty) and scratchpad_compact (full snapshot). */
static void write_section_jsonl(FILE *f, const scratchpad_section_t *s) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", s->name);
    cJSON_AddNumberToObject(obj, "priority", s->priority);
    cJSON_AddStringToObject(obj, "content", s->content);
    char *line = cJSON_PrintUnformatted(obj);
    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }
    cJSON_Delete(obj);
}

int scratchpad_save(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return -1;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/scratchpad.jsonl", session_dir);

    pthread_mutex_lock(&sp->mtx);
    FILE *f = fopen(path, "a");  /* append mode */
    if (!f) { pthread_mutex_unlock(&sp->mtx); return -1; }

    /* Write op:clear entries for sections cleared since last save */
    for (int i = 0; i < sp->n_cleared; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", sp->cleared_names[i]);
        cJSON_AddStringToObject(obj, "op", "clear");
        char *line = cJSON_PrintUnformatted(obj);
        if (line) {
            fprintf(f, "%s\n", line);
            free(line);
        }
        cJSON_Delete(obj);
        free(sp->cleared_names[i]);
    }
    sp->n_cleared = 0;

    /* Write dirty sections (append-only — only changed sections) */
    for (int i = 0; i < sp->count; i++) {
        if (!sp->sections[i].dirty) continue;
        write_section_jsonl(f, &sp->sections[i]);
        sp->sections[i].dirty = 0;
    }

    fclose(f);
    pthread_mutex_unlock(&sp->mtx);
    return 0;
}

int scratchpad_load(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return -1;

    /* Try JSONL format first */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/scratchpad.jsonl", session_dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Fall back to legacy scratchpad.md */
        return scratchpad_load_legacy(sp, session_dir);
    }

    /* Parse JSONL: last entry per section name wins.
     * FIX BUG#12: heap-allocate the line buffer instead of 1MB on the stack,
     * which risks stack overflow especially in worker threads. */
    size_t line_cap = 1024 * 1024;  /* 1MB max per line */
    char *line = malloc(line_cap);
    if (!line) { fclose(f); return -1; }
    while (fgets(line, (int)line_cap, f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        cJSON *name_j = cJSON_GetObjectItem(obj, "name");
        if (!name_j || !cJSON_IsString(name_j)) {
            cJSON_Delete(obj);
            continue;
        }
        const char *name = name_j->valuestring;

        cJSON *op_j = cJSON_GetObjectItem(obj, "op");
        if (op_j && cJSON_IsString(op_j) &&
            strcmp(op_j->valuestring, "clear") == 0) {
            scratchpad_clear(sp, name);
        } else {
            cJSON *pri_j = cJSON_GetObjectItem(obj, "priority");
            cJSON *content_j = cJSON_GetObjectItem(obj, "content");
            int pri = (pri_j && cJSON_IsNumber(pri_j))
                      ? (int)cJSON_GetNumberValue(pri_j) : 5;
            const char *content = (content_j && cJSON_IsString(content_j))
                                  ? content_j->valuestring : "";
            scratchpad_write(sp, name, content, pri);
        }
        cJSON_Delete(obj);
    }
    free(line);
    fclose(f);

    /* Mark all loaded sections as not dirty (they came from disk) */
    for (int i = 0; i < sp->count; i++)
        sp->sections[i].dirty = 0;
    /* Clear any clears tracked during load */
    for (int i = 0; i < sp->n_cleared; i++)
        free(sp->cleared_names[i]);
    sp->n_cleared = 0;

    return 0;
}

void scratchpad_compact(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/scratchpad.jsonl", session_dir);

    pthread_mutex_lock(&sp->mtx);
    FILE *f = fopen(path, "w");  /* truncate */
    if (!f) { pthread_mutex_unlock(&sp->mtx); return; }

    /* Write one line per live section (cleared sections omitted) */
    for (int i = 0; i < sp->count; i++) {
        write_section_jsonl(f, &sp->sections[i]);
        sp->sections[i].dirty = 0;
    }

    fclose(f);
    /* Clear pending clears since we just wrote a clean snapshot */
    for (int i = 0; i < sp->n_cleared; i++)
        free(sp->cleared_names[i]);
    sp->n_cleared = 0;
    pthread_mutex_unlock(&sp->mtx);
}

int scratchpad_parse(scratchpad_t *sp, const char *text,
                     const char *fallback_name, int default_priority) {
    if (!text || !*text) return -1;

    /* Check if the text contains "## " section headers.
     * Accept text that starts with "## " or contains "\n## ". */
    const char *first_hdr = NULL;
    if (strncmp(text, "## ", 3) == 0) {
        first_hdr = text;
    } else {
        first_hdr = strstr(text, "\n## ");
        if (first_hdr) first_hdr++;  /* skip the \n, point at ## */
    }

    if (!first_hdr) {
        /* No section headers — store as single fallback section.
         * FIX BUG#4: use scratchpad_reset() instead of scratchpad_free()
         * to avoid destroying the mutex before scratchpad_write() locks it. */
        scratchpad_reset(sp);
        scratchpad_write(sp, fallback_name ? fallback_name : "pruned",
                         text, default_priority);
        return 0;
    }

    /* Parse structured content into sections.
     * FIX BUG#4: use scratchpad_reset() to preserve the mutex. */
    scratchpad_reset(sp);
    int count = 0;
    const char *p = first_hdr;

    while (p && *p) {
        /* Expect "## " at current position */
        if (strncmp(p, "## ", 3) != 0) {
            /* Skip to next "## " header */
            const char *next = strstr(p, "\n## ");
            if (next) { p = next + 1; continue; }
            break;
        }

        /* Extract section name */
        const char *hdr = p + 3;
        const char *hdr_end = strchr(hdr, '\n');
        if (!hdr_end) hdr_end = hdr + strlen(hdr);

        char sec_name[256];
        size_t nlen = (size_t)(hdr_end - hdr);
        if (nlen >= sizeof(sec_name)) nlen = sizeof(sec_name) - 1;
        memcpy(sec_name, hdr, nlen);
        sec_name[nlen] = '\0';

        /* Extract body until next "## " or end */
        const char *body = (*hdr_end) ? hdr_end + 1 : hdr_end;
        const char *body_end = strstr(body, "\n## ");
        if (!body_end) body_end = body + strlen(body);

        /* Trim trailing whitespace */
        while (body_end > body &&
               (*(body_end - 1) == '\n' || *(body_end - 1) == ' '))
            body_end--;

        size_t blen = (size_t)(body_end - body);
        char *sec_content = malloc(blen + 1);
        if (sec_content) {
            memcpy(sec_content, body, blen);
            sec_content[blen] = '\0';
            scratchpad_write(sp, sec_name, sec_content, default_priority);
            free(sec_content);
            count++;
        }

        /* Advance to next section */
        const char *next = strstr(body, "\n## ");
        if (next) {
            p = next + 1;  /* skip \n, point at ## */
        } else {
            break;
        }
    }

    if (count == 0) {
        /* Parsing found headers but extracted nothing — fallback */
        scratchpad_write(sp, fallback_name ? fallback_name : "pruned",
                         text, default_priority);
    }

    return 0;
}
