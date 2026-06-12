#include "scratchpad.h"
#include "str.h"
#include "nash_limits.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void scratchpad_init(scratchpad_t *sp) {
    memset(sp, 0, sizeof(*sp));
    sp->cap = SCRATCHPAD_INIT_CAP;
    sp->sections = calloc((size_t)sp->cap, sizeof(scratchpad_section_t));
}

void scratchpad_free(scratchpad_t *sp) {
    for (int i = 0; i < sp->count; i++) {
        free(sp->sections[i].name);
        free(sp->sections[i].content);
    }
    free(sp->sections);
    sp->sections = NULL;
    sp->count = 0;
    sp->cap = 0;
}

void scratchpad_move(scratchpad_t *dst, scratchpad_t *src) {
    scratchpad_free(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

/* Ensure capacity for at least one more section. */
static int scratchpad_grow(scratchpad_t *sp) {
    if (sp->count < sp->cap) return 0;
    int new_cap = sp->cap ? sp->cap * 2 : SCRATCHPAD_INIT_CAP;
    scratchpad_section_t *new_s = realloc(sp->sections,
                                          (size_t)new_cap * sizeof(scratchpad_section_t));
    if (!new_s) return -1;
    memset(new_s + sp->cap, 0, (size_t)(new_cap - sp->cap) * sizeof(scratchpad_section_t));
    sp->sections = new_s;
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

    int idx = scratchpad_find(sp, name);
    if (idx >= 0) {
        /* Overwrite existing section */
        free(sp->sections[idx].content);
        sp->sections[idx].content = strdup(content);
        sp->sections[idx].priority = priority;
        return 0;
    }
    if (scratchpad_grow(sp) < 0)
        return -1;  /* allocation failed */

    sp->sections[sp->count].name = strdup(name);
    sp->sections[sp->count].content = strdup(content);
    sp->sections[sp->count].priority = priority;
    sp->count++;
    return 0;
}

int scratchpad_append(scratchpad_t *sp, const char *name, const char *content, int priority) {
    int idx = scratchpad_find(sp, name);
    if (idx >= 0) {
        /* Append to existing */
        size_t old_len = strlen(sp->sections[idx].content);
        size_t add_len = strlen(content);
        char *combined = malloc(old_len + add_len + 2);  /* +newline+nul */
        if (!combined) return -1;
        memcpy(combined, sp->sections[idx].content, old_len);
        combined[old_len] = '\n';
        memcpy(combined + old_len + 1, content, add_len);
        combined[old_len + 1 + add_len] = '\0';
        free(sp->sections[idx].content);
        sp->sections[idx].content = combined;
        return 0;
    }
    /* Create new section */
    return scratchpad_write(sp, name, content, priority);
}

int scratchpad_clear(scratchpad_t *sp, const char *name) {
    int idx = scratchpad_find(sp, name);
    if (idx < 0) return -1;

    free(sp->sections[idx].name);
    free(sp->sections[idx].content);

    /* Shift remaining sections down */
    for (int i = idx; i < sp->count - 1; i++)
        sp->sections[i] = sp->sections[i + 1];
    sp->count--;
    return 0;
}

/* Compare sections by priority for qsort (lower priority number = first) */
static int section_cmp(const void *a, const void *b) {
    const scratchpad_section_t *sa = a;
    const scratchpad_section_t *sb = b;
    return sa->priority - sb->priority;
}

char *scratchpad_serialize(scratchpad_t *sp) {
    if (sp->count == 0) return NULL;

    /* Sort a copy by priority */
    scratchpad_section_t *sorted = malloc((size_t)sp->count * sizeof(scratchpad_section_t));
    if (!sorted) return NULL;
    memcpy(sorted, sp->sections, (size_t)sp->count * sizeof(scratchpad_section_t));
    qsort(sorted, (size_t)sp->count, sizeof(scratchpad_section_t), section_cmp);

    str_t out = str_new(2048);
    for (int i = 0; i < sp->count; i++) {
        str_appendf(&out, "## %s\n%s\n\n", sorted[i].name, sorted[i].content);
    }
    free(sorted);
    return str_steal(&out);
}

char *scratchpad_serialize_budget(scratchpad_t *sp, size_t max_chars) {
    if (sp->count == 0) return NULL;

    /* Sort a copy by priority (ascending = highest priority first) */
    scratchpad_section_t *sorted = malloc((size_t)sp->count * sizeof(scratchpad_section_t));
    if (!sorted) return NULL;
    memcpy(sorted, sp->sections, (size_t)sp->count * sizeof(scratchpad_section_t));
    qsort(sorted, sp->count, sizeof(scratchpad_section_t), section_cmp);

    str_t out = str_new(max_chars > 4096 ? 4096 : max_chars);
    for (int i = 0; i < sp->count; i++) {
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
            /* Truncate content to fit budget (defensive: guard against underflow) */
            size_t avail = (remaining > header_len + 12) ? (remaining - header_len - 12) : 0;
            if (avail > 0) str_append(&out, sorted[i].content, avail);
            str_append_cstr(&out, "\n[truncated]");
        }
        str_append_cstr(&out, "\n\n");
    }

    free(sorted);
    if (out.len == 0) {
        str_free(&out);
        return NULL;
    }
    return str_steal(&out);
}

int scratchpad_save(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/scratchpad.md", session_dir);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < sp->count; i++) {
        fprintf(f, "<!-- priority:%d -->\n## %s\n%s\n\n",
                sp->sections[i].priority, sp->sections[i].name,
                sp->sections[i].content);
    }
    fclose(f);
    return 0;
}

int scratchpad_load(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/scratchpad.md", session_dir);

    char *buf = slurp_file(path, NULL);
    if (!buf) return -1;
    if (buf[0] == '\0') { free(buf); return 0; }

    /* Parse sections from the file format:
     * <!-- priority:N -->
     * ## section_name
     * content...
     */
    scratchpad_free(sp);  /* clear any existing sections */

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

    free(buf);
    return 0;
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
        /* No section headers — store as single fallback section */
        scratchpad_free(sp);
        scratchpad_write(sp, fallback_name ? fallback_name : "pruned",
                         text, default_priority);
        return 0;
    }

    /* Parse structured content into sections */
    scratchpad_free(sp);
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
