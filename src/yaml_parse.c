/*
 * Minimal YAML parser for nash playbook files.
 *
 * Supports the subset needed for playbooks:
 *   - Mappings (key: value)
 *   - Sequences (- item)
 *   - Block scalars (| literal, > folded)
 *   - Quoted strings (single and double)
 *   - Unquoted scalars (strings, ints, bools)
 *   - Comments (#)
 *   - Nested structures via indentation
 *
 * Does NOT support: anchors, aliases, tags, flow collections ({}, []),
 * multi-document (---), complex keys, merge keys (<<).
 */

#include "yaml_parse.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Node allocation ─────────────────────────────────── */

static yaml_node_t *node_new(yaml_type_t type) {
    yaml_node_t *n = calloc(1, sizeof(yaml_node_t));
    if (!n) return NULL;
    n->type = type;
    return n;
}

static yaml_node_t *node_scalar(const char *val) {
    yaml_node_t *n = node_new(YAML_SCALAR);
    if (!n) return NULL;
    n->scalar = val ? strdup(val) : strdup("");
    return n;
}

static void mapping_add(yaml_node_t *map, const char *key, yaml_node_t *val) {
    if (!map || map->type != YAML_MAPPING || !val) return;
    if (map->n_children >= map->cap_children) {
        int newcap = map->cap_children ? map->cap_children * 2 : 8;
        map->keys = realloc(map->keys, newcap * sizeof(char *));
        map->values = realloc(map->values, newcap * sizeof(yaml_node_t *));
        map->cap_children = newcap;
    }
    map->keys[map->n_children] = strdup(key);
    map->values[map->n_children] = val;
    map->n_children++;
}

static void sequence_add(yaml_node_t *seq, yaml_node_t *item) {
    if (!seq || seq->type != YAML_SEQUENCE || !item) return;
    if (seq->n_items >= seq->cap_items) {
        int newcap = seq->cap_items ? seq->cap_items * 2 : 8;
        seq->items = realloc(seq->items, newcap * sizeof(yaml_node_t *));
        seq->cap_items = newcap;
    }
    seq->items[seq->n_items++] = item;
}

void yaml_free(yaml_node_t *node) {
    if (!node) return;
    free(node->scalar);
    if (node->type == YAML_MAPPING) {
        for (int i = 0; i < node->n_children; i++) {
            free(node->keys[i]);
            yaml_free(node->values[i]);
        }
        free(node->keys);
        free(node->values);
    }
    if (node->type == YAML_SEQUENCE) {
        for (int i = 0; i < node->n_items; i++)
            yaml_free(node->items[i]);
        free(node->items);
    }
    free(node);
}

/* ── Line-based parser ───────────────────────────────── */

typedef struct {
    const char **lines;    /* array of line pointers */
    int         *indents;  /* indentation level per line */
    int          n_lines;
    int          pos;      /* current line index */
} parser_t;

/* Calculate indentation (number of leading spaces) */
static int calc_indent(const char *line) {
    int n = 0;
    while (line[n] == ' ') n++;
    return n;
}

/* Check if a line is blank or comment-only */
static int is_blank_or_comment(const char *line) {
    while (*line == ' ') line++;
    return *line == '\0' || *line == '#' || *line == '\n';
}

/* Strip trailing whitespace and newline in-place */
static char *strip_trailing(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' '))
        s[--len] = '\0';
    return s;
}

/* Strip inline comment: find # preceded by space (not inside quotes) */
static void strip_comment(char *s) {
    int in_sq = 0, in_dq = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\'' && !in_dq) in_sq = !in_sq;
        else if (s[i] == '\"' && !in_sq) in_dq = !in_dq;
        else if (s[i] == '#' && !in_sq && !in_dq && i > 0 && s[i-1] == ' ') {
            s[i-1] = '\0';
            strip_trailing(s);
            return;
        }
    }
}

/* Unquote a scalar value */
static char *unquote(const char *s) {
    int len = (int)strlen(s);
    if (len >= 2) {
        if ((s[0] == '\"' && s[len-1] == '\"') ||
            (s[0] == '\'' && s[len-1] == '\'')) {
            char *r = malloc(len - 1);
            memcpy(r, s + 1, len - 2);
            r[len - 2] = '\0';
            return r;
        }
    }
    return strdup(s);
}

/* Split input into lines */
static parser_t parser_init(const char *input) {
    parser_t p = {0};
    /* Count lines */
    int n = 1;
    for (const char *c = input; *c; c++)
        if (*c == '\n') n++;

    p.lines = malloc(n * sizeof(char *));
    p.indents = malloc(n * sizeof(int));

    /* Copy and split lines */
    char *buf = strdup(input);
    char *line = buf;
    int idx = 0;
    for (char *c = buf; ; c++) {
        if (*c == '\n' || *c == '\0') {
            int end = (*c == '\0');
            *c = '\0';
            /* Skip blank/comment lines for the parser, but keep them for block scalars */
            char *trimmed = strdup(line);
            strip_trailing(trimmed);
            if (!is_blank_or_comment(trimmed) || idx == 0) {
                p.lines = realloc(p.lines, (idx + 1) * sizeof(char *));
                p.indents = realloc(p.indents, (idx + 1) * sizeof(int));
                p.indents[idx] = calc_indent(trimmed);
                p.lines[idx] = trimmed;
                idx++;
            } else {
                free(trimmed);
            }
            if (end) break;
            line = c + 1;
        }
    }
    free(buf);
    p.n_lines = idx;
    p.pos = 0;
    return p;
}

static void parser_free(parser_t *p) {
    for (int i = 0; i < p->n_lines; i++)
        free((char *)p->lines[i]);
    free(p->lines);
    free(p->indents);
}

/* Forward declarations */
static yaml_node_t *parse_value(parser_t *p, int min_indent);
static yaml_node_t *parse_mapping(parser_t *p, int indent);
static yaml_node_t *parse_sequence(parser_t *p, int indent);

/* Read a block scalar (| or >) starting at current position.
 * The block scalar indicator is on the previous line.
 * Collects all lines with indent > base_indent. */
static char *read_block_scalar(parser_t *p, int base_indent, int literal) {
    /* Find the indent of the first content line */
    int content_indent = -1;
    int start = p->pos;

    /* Peek ahead to find content indent */
    for (int i = start; i < p->n_lines; i++) {
        const char *ln = p->lines[i];
        int ind = p->indents[i];
        /* Skip blank lines */
        int blank = 1;
        for (const char *c = ln; *c; c++) {
            if (*c != ' ') { blank = 0; break; }
        }
        if (blank) continue;
        if (ind <= base_indent) break;
        content_indent = ind;
        break;
    }

    if (content_indent < 0) return strdup("");

    /* Collect lines */
    size_t cap = 1024, len = 0;
    char *result = malloc(cap);
    result[0] = '\0';

    while (p->pos < p->n_lines) {
        const char *ln = p->lines[p->pos];
        int ind = p->indents[p->pos];

        /* Check if this line belongs to the block */
        int blank = 1;
        for (const char *c = ln; *c; c++) {
            if (*c != ' ') { blank = 0; break; }
        }

        if (!blank && ind < content_indent) break;

        /* Strip the content indent */
        const char *content = ln;
        int strip = 0;
        while (*content == ' ' && strip < content_indent) {
            content++;
            strip++;
        }

        size_t clen = strlen(content);
        while (len + clen + 2 > cap) {
            cap *= 2;
            result = realloc(result, cap);
        }

        if (literal) {
            /* | : preserve newlines exactly */
            memcpy(result + len, content, clen);
            len += clen;
            result[len++] = '\n';
        } else {
            /* > : fold newlines into spaces (except blank lines) */
            if (blank) {
                result[len++] = '\n';
            } else {
                if (len > 0 && result[len-1] != '\n') {
                    result[len++] = ' ';
                }
                memcpy(result + len, content, clen);
                len += clen;
            }
        }
        result[len] = '\0';
        p->pos++;
    }

    /* Trim trailing newline for folded */
    if (!literal && len > 0 && result[len-1] == ' ')
        result[--len] = '\0';

    /* Always ensure trailing newline for literal */
    if (literal && len > 0 && result[len-1] != '\n') {
        if (len + 1 >= cap) result = realloc(result, cap + 2);
        result[len++] = '\n';
        result[len] = '\0';
    }

    return result;
}

/* Parse a value: could be a scalar, mapping, or sequence */
static yaml_node_t *parse_value(parser_t *p, int min_indent) {
    if (p->pos >= p->n_lines) return node_scalar("");

    const char *line = p->lines[p->pos];
    int indent = p->indents[p->pos];

    if (indent < min_indent) return node_scalar("");

    /* Check if this is a sequence */
    const char *trimmed = line + indent;
    if (trimmed[0] == '-' && (trimmed[1] == ' ' || trimmed[1] == '\0')) {
        return parse_sequence(p, indent);
    }

    /* Check if this is a mapping (has key: pattern) */
    const char *colon = strchr(trimmed, ':');
    if (colon && (colon[1] == ' ' || colon[1] == '\0' || colon[1] == '\n')) {
        return parse_mapping(p, indent);
    }

    /* Plain scalar */
    char *val = unquote(trimmed);
    yaml_node_t *n = node_scalar(val);
    free(val);
    p->pos++;
    return n;
}

/* Parse a mapping at a given indentation level */
static yaml_node_t *parse_mapping(parser_t *p, int indent) {
    yaml_node_t *map = node_new(YAML_MAPPING);

    while (p->pos < p->n_lines && p->indents[p->pos] == indent) {
        const char *line = p->lines[p->pos];
        const char *trimmed = line + indent;

        /* Must be a key: value line */
        const char *colon = strchr(trimmed, ':');
        if (!colon) break;

        /* Check this is actually a key (colon followed by space, EOL, or block indicator) */
        if (colon[1] != ' ' && colon[1] != '\0' && colon[1] != '\n' &&
            colon[1] != '|' && colon[1] != '>') break;

        /* Extract key */
        int key_len = (int)(colon - trimmed);
        char *key = malloc(key_len + 1);
        memcpy(key, trimmed, key_len);
        key[key_len] = '\0';

        /* Extract value */
        const char *val_start = colon + 1;
        while (*val_start == ' ') val_start++;

        yaml_node_t *val = NULL;

        if (*val_start == '\0' || *val_start == '\n') {
            /* Value is on next line(s) — could be mapping, sequence, or block scalar */
            p->pos++;
            val = parse_value(p, indent + 1);
        } else if (*val_start == '|' || *val_start == '>') {
            /* Block scalar */
            int literal = (*val_start == '|');
            p->pos++;
            char *block = read_block_scalar(p, indent, literal);
            val = node_scalar(block);
            free(block);
        } else {
            /* Inline scalar value */
            char *line_copy = strdup(val_start);
            strip_comment(line_copy);
            strip_trailing(line_copy);
            char *uq = unquote(line_copy);
            val = node_scalar(uq);
            free(uq);
            free(line_copy);
            p->pos++;
        }

        mapping_add(map, key, val);
        free(key);
    }

    return map;
}

/* Parse a sequence at a given indentation level */
static yaml_node_t *parse_sequence(parser_t *p, int indent) {
    yaml_node_t *seq = node_new(YAML_SEQUENCE);

    while (p->pos < p->n_lines && p->indents[p->pos] == indent) {
        const char *line = p->lines[p->pos];
        const char *trimmed = line + indent;

        if (trimmed[0] != '-') break;
        if (trimmed[1] != ' ' && trimmed[1] != '\0') break;

        const char *item_start = trimmed + 1;
        while (*item_start == ' ') item_start++;

        if (*item_start == '\0') {
            /* Item value is on next line(s) */
            p->pos++;
            yaml_node_t *item = parse_value(p, indent + 1);
            sequence_add(seq, item);
        } else {
            /* Check if item is a mapping (- key: value) */
            const char *colon = strchr(item_start, ':');
            if (colon && (colon[1] == ' ' || colon[1] == '\0' || colon[1] == '\n' ||
                          colon[1] == '|' || colon[1] == '>')) {
                /* Inline mapping item — the content starts at indent + 2 */
                /* We need to parse this as a mapping with the first key on this line */
                yaml_node_t *item_map = node_new(YAML_MAPPING);

                /* Parse first key-value from this line */
                int klen = (int)(colon - item_start);
                char *k = malloc(klen + 1);
                memcpy(k, item_start, klen);
                k[klen] = '\0';

                const char *vs = colon + 1;
                while (*vs == ' ') vs++;

                yaml_node_t *v = NULL;
                if (*vs == '\0' || *vs == '\n') {
                    p->pos++;
                    v = parse_value(p, indent + 2);
                } else if (*vs == '|' || *vs == '>') {
                    int lit = (*vs == '|');
                    p->pos++;
                    char *block = read_block_scalar(p, indent + 2, lit);
                    v = node_scalar(block);
                    free(block);
                } else {
                    char *vc = strdup(vs);
                    strip_comment(vc);
                    strip_trailing(vc);
                    char *uq = unquote(vc);
                    v = node_scalar(uq);
                    free(uq);
                    free(vc);
                    p->pos++;
                }

                mapping_add(item_map, k, v);
                free(k);

                /* Continue reading more keys at indent + 2 */
                int item_indent = indent + 2;
                while (p->pos < p->n_lines && p->indents[p->pos] >= item_indent) {
                    const char *ln = p->lines[p->pos];
                    const char *tr = ln + p->indents[p->pos];

                    if (p->indents[p->pos] != item_indent) {
                        /* Deeper indent — belongs to previous value, skip */
                        break;
                    }

                    const char *c2 = strchr(tr, ':');
                    if (!c2 || (c2[1] != ' ' && c2[1] != '\0' && c2[1] != '\n' &&
                                c2[1] != '|' && c2[1] != '>')) break;

                    int k2len = (int)(c2 - tr);
                    char *k2 = malloc(k2len + 1);
                    memcpy(k2, tr, k2len);
                    k2[k2len] = '\0';

                    const char *v2s = c2 + 1;
                    while (*v2s == ' ') v2s++;

                    yaml_node_t *v2 = NULL;
                    if (*v2s == '\0' || *v2s == '\n') {
                        p->pos++;
                        v2 = parse_value(p, item_indent + 1);
                    } else if (*v2s == '|' || *v2s == '>') {
                        int lit2 = (*v2s == '|');
                        p->pos++;
                        char *block2 = read_block_scalar(p, item_indent, lit2);
                        v2 = node_scalar(block2);
                        free(block2);
                    } else {
                        char *v2c = strdup(v2s);
                        strip_comment(v2c);
                        strip_trailing(v2c);
                        char *uq2 = unquote(v2c);
                        v2 = node_scalar(uq2);
                        free(uq2);
                        free(v2c);
                        p->pos++;
                    }

                    mapping_add(item_map, k2, v2);
                    free(k2);
                }

                sequence_add(seq, item_map);
            } else {
                /* Plain scalar item */
                char *ic = strdup(item_start);
                strip_comment(ic);
                strip_trailing(ic);
                char *uq = unquote(ic);
                yaml_node_t *item = node_scalar(uq);
                free(uq);
                free(ic);
                p->pos++;
                sequence_add(seq, item);
            }
        }
    }

    return seq;
}

/* ── Public API ──────────────────────────────────────── */

yaml_node_t *yaml_parse(const char *input) {
    if (!input || !*input) return NULL;
    parser_t p = parser_init(input);
    yaml_node_t *root = parse_value(&p, 0);
    parser_free(&p);
    return root;
}

yaml_node_t *yaml_parse_file(const char *path) {
    size_t len = 0;
    char *buf = slurp_file(path, &len);
    if (!buf || len == 0) { free(buf); return NULL; }

    yaml_node_t *root = yaml_parse(buf);
    free(buf);
    return root;
}

yaml_node_t *yaml_get(yaml_node_t *node, const char *key) {
    if (!node || node->type != YAML_MAPPING || !key) return NULL;
    for (int i = 0; i < node->n_children; i++) {
        if (strcmp(node->keys[i], key) == 0)
            return node->values[i];
    }
    return NULL;
}

const char *yaml_str(yaml_node_t *node) {
    if (!node || node->type != YAML_SCALAR) return NULL;
    return node->scalar;
}

int yaml_int(yaml_node_t *node, int def) {
    const char *s = yaml_str(node);
    if (!s) return def;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return def;
    return (int)v;
}

int yaml_bool(yaml_node_t *node, int def) {
    const char *s = yaml_str(node);
    if (!s) return def;
    if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
        strcmp(s, "True") == 0 || strcmp(s, "Yes") == 0 ||
        strcmp(s, "TRUE") == 0 || strcmp(s, "YES") == 0 ||
        strcmp(s, "on") == 0 || strcmp(s, "On") == 0 ||
        strcmp(s, "ON") == 0) return 1;
    if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
        strcmp(s, "False") == 0 || strcmp(s, "No") == 0 ||
        strcmp(s, "FALSE") == 0 || strcmp(s, "NO") == 0 ||
        strcmp(s, "off") == 0 || strcmp(s, "Off") == 0 ||
        strcmp(s, "OFF") == 0) return 0;
    return def;
}

int yaml_len(yaml_node_t *node) {
    if (!node || node->type != YAML_SEQUENCE) return 0;
    return node->n_items;
}

yaml_node_t *yaml_item(yaml_node_t *node, int index) {
    if (!node || node->type != YAML_SEQUENCE) return NULL;
    if (index < 0 || index >= node->n_items) return NULL;
    return node->items[index];
}
