#ifndef SCRATCHPAD_H
#define SCRATCHPAD_H

#include <stddef.h>

/* ── Section-based scratchpad (GDN-2 inspired) ──────────── */
/* Each section has independent name, content, and priority.
 * Operations: write, append, read, clear, list.
 * Priority determines compression/eviction order under context pressure. */

#define SCRATCHPAD_INIT_CAP 32

typedef struct {
    char *name;       /* section name (e.g. "findings", "plan", "status") */
    char *content;    /* section content (owned) */
    int   priority;   /* 1 = highest priority, 9 = lowest. Default: 5 */
} scratchpad_section_t;

typedef struct {
    scratchpad_section_t *sections;  /* dynamically allocated array */
    int count;
    int cap;                         /* allocated capacity */
} scratchpad_t;

/* Scratchpad lifecycle */
void scratchpad_init(scratchpad_t *sp);
void scratchpad_free(scratchpad_t *sp);

/* Move ownership: dst takes all sections from src, src is zeroed.
 * Any existing sections in dst are freed first. */
void scratchpad_move(scratchpad_t *dst, scratchpad_t *src);

/* Find section by name. Returns index or -1. */
int scratchpad_find(scratchpad_t *sp, const char *name);

/* Write (create/overwrite) a section. Returns 0 on success. */
int scratchpad_write(scratchpad_t *sp, const char *name, const char *content, int priority);

/* Append to a section (creates if not exists). Returns 0 on success. */
int scratchpad_append(scratchpad_t *sp, const char *name, const char *content, int priority);

/* Clear (delete) a section. Returns 0 on success, -1 if not found. */
int scratchpad_clear(scratchpad_t *sp, const char *name);

/* Serialize all sections to a single string for context injection.
 * Format: "## section_name\ncontent\n\n## section2\ncontent2\n"
 * Sections are ordered by priority (1 first, 9 last).
 * Caller must free. Returns NULL if empty. */
char *scratchpad_serialize(scratchpad_t *sp);

/* Serialize with a max_chars budget. Low-priority sections are truncated/dropped first.
 * Caller must free. Returns NULL if empty. */
char *scratchpad_serialize_budget(scratchpad_t *sp, size_t max_chars);

/* Persist scratchpad to disk (session_dir/scratchpad.md). */
int scratchpad_save(scratchpad_t *sp, const char *session_dir);

/* Load scratchpad from disk. Returns 0 on success. */
int scratchpad_load(scratchpad_t *sp, const char *session_dir);

/* Parse serialized scratchpad text (## section headers) into sections.
 * Clears any existing sections in sp first. If the text contains "## "
 * section headers, parses them into individual sections. Otherwise stores
 * the entire text as a single section named fallback_name.
 * default_priority is used when priority can't be determined from text. */
int scratchpad_parse(scratchpad_t *sp, const char *text,
                     const char *fallback_name, int default_priority);

#endif
