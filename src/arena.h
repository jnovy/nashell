#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct arena_block {
    char   *base;
    size_t  used;
    size_t  cap;
    struct arena_block *next;
} arena_block_t;

typedef struct {
    arena_block_t *head;
    arena_block_t *current;
} arena_t;

arena_t *arena_new(size_t initial_cap);
void    *arena_alloc(arena_t *a, size_t size);
char    *arena_strdup(arena_t *a, const char *s);
char    *arena_sprintf(arena_t *a, const char *fmt, ...);
void     arena_reset(arena_t *a);
void     arena_free(arena_t *a);

#endif
