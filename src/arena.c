#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static arena_block_t *block_new(size_t cap) {
    arena_block_t *b = malloc(sizeof(arena_block_t));
    if (!b) return NULL;
    b->base = malloc(cap);
    if (!b->base) { free(b); return NULL; }
    b->used = 0;
    b->cap  = cap;
    b->next = NULL;
    return b;
}

arena_t *arena_new(size_t initial_cap) {
    arena_t *a = malloc(sizeof(arena_t));
    if (!a) return NULL;
    a->head = block_new(initial_cap > 0 ? initial_cap : 4096);
    if (!a->head) { free(a); return NULL; }
    a->current = a->head;
    return a;
}

void *arena_alloc(arena_t *a, size_t size) {
    /* align to 8 bytes */
    size = (size + 7) & ~(size_t)7;
    if (a->current->used + size > a->current->cap) {
        size_t new_cap = a->current->cap * 2;
        if (new_cap < size) new_cap = size * 2;
        arena_block_t *b = block_new(new_cap);
        a->current->next = b;
        a->current = b;
    }
    void *p = a->current->base + a->current->used;
    a->current->used += size;
    return p;
}

char *arena_strdup(arena_t *a, const char *s) {
    size_t len = strlen(s) + 1;
    char *p = arena_alloc(a, len);
    memcpy(p, s, len);
    return p;
}

char *arena_sprintf(arena_t *a, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void arena_reset(arena_t *a) {
    arena_block_t *b = a->head;
    while (b) {
        b->used = 0;
        b = b->next;
    }
    a->current = a->head;
}

void arena_free(arena_t *a) {
    arena_block_t *b = a->head;
    while (b) {
        arena_block_t *next = b->next;
        free(b->base);
        free(b);
        b = next;
    }
    free(a);
}
