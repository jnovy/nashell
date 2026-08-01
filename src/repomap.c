/* repomap.c — Structural codebase context for LLM consumption.
 *
 * Implements the 6-phase Aider-style repo map algorithm without tree-sitter:
 *   Phase 1: Symbol extraction via line-based C/Python/Shell parsing
 *   Phase 2: Directed graph building (file → file, weighted edges)
 *   Phase 3: Personalized PageRank (iterative power method)
 *   Phase 4: Rank distribution to individual (file, symbol) pairs
 *   Phase 5: Budget fitting via greedy ranked selection
 *   Phase 6: Scope-aware rendering with ⋮ elision
 */

#include "repomap.h"
#include "str.h"
#include "nash_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Limits ───────────────────────────────────────────────────────── */
#define RM_MAX_FILES      512    /* max files to index */
#define RM_MAX_TAGS       8192   /* max tags (definitions + references) */
#define RM_MAX_EDGES      16384  /* max graph edges */
#define RM_MAX_IDENT_LEN  128    /* max identifier length */
#define RM_MAX_LINE_LEN   1024   /* max line length for parsing */
#define RM_DEFAULT_CHARS  8000   /* default output budget */
#define RM_PAGERANK_ITERS 20     /* PageRank iterations */
#define RM_PAGERANK_DAMP  0.85   /* PageRank damping factor */

/* ── Phase 1: Symbol Extraction ───────────────────────────────────── */

typedef enum {
    TAG_DEFINITION,
    TAG_REFERENCE
} tag_kind_t;

typedef enum {
    SYM_FUNCTION,
    SYM_TYPE,       /* struct, typedef, enum */
    SYM_MACRO,      /* #define */
    SYM_VARIABLE,
    SYM_INCLUDE     /* #include reference */
} sym_type_t;

typedef struct {
    int       file_idx;     /* index into files array */
    int       line;         /* 1-based line number */
    char      name[RM_MAX_IDENT_LEN];
    tag_kind_t kind;
    sym_type_t sym_type;
} tag_t;

typedef struct {
    char     path[NASH_PATH_MAX];  /* relative path */
    int      n_lines;              /* total lines in file */
    char   **lines;                /* all lines (for rendering) */
} rm_file_t;

/* State for the entire repo map computation */
typedef struct {
    rm_file_t  files[RM_MAX_FILES];
    int        n_files;
    tag_t      tags[RM_MAX_TAGS];
    int        n_tags;
} rm_state_t;

/* ── File Discovery ───────────────────────────────────────────────── */

/* Check if a file extension is one we can parse */
static int is_parseable(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    /* C/C++ */
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0) return 1;
    if (strcmp(dot, ".cpp") == 0 || strcmp(dot, ".hpp") == 0) return 1;
    if (strcmp(dot, ".cc") == 0 || strcmp(dot, ".hh") == 0) return 1;
    /* Python */
    if (strcmp(dot, ".py") == 0) return 1;
    /* Shell */
    if (strcmp(dot, ".sh") == 0) return 1;
    /* JavaScript/TypeScript */
    if (strcmp(dot, ".js") == 0 || strcmp(dot, ".ts") == 0) return 1;
    if (strcmp(dot, ".jsx") == 0 || strcmp(dot, ".tsx") == 0) return 1;
    /* Go */
    if (strcmp(dot, ".go") == 0) return 1;
    /* Rust */
    if (strcmp(dot, ".rs") == 0) return 1;
    /* Java */
    if (strcmp(dot, ".java") == 0) return 1;
    /* Makefile, YAML, TOML, Markdown */
    if (strcmp(dot, ".mk") == 0) return 1;
    if (strcmp(dot, ".yaml") == 0 || strcmp(dot, ".yml") == 0) return 1;
    if (strcmp(dot, ".toml") == 0) return 1;
    if (strcmp(dot, ".md") == 0) return 1;
    return 0;
}

/* Check if the base filename (without directory) is a Makefile variant */
static int is_makefile(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return (strcmp(base, "Makefile") == 0 ||
            strcmp(base, "makefile") == 0 ||
            strcmp(base, "GNUmakefile") == 0);
}

/* Should this directory be skipped? */
static int skip_dir(const char *name) {
    if (name[0] == '.') return 1;  /* .git, .nash, etc. */
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "__pycache__") == 0) return 1;
    if (strcmp(name, "vendor") == 0) return 1;
    if (strcmp(name, "build") == 0) return 1;
    if (strcmp(name, "dist") == 0) return 1;
    if (strcmp(name, "target") == 0) return 1;
    return 0;
}

/* Recursively discover source files via git ls-files or directory walk */
static void discover_files(rm_state_t *st, const char *root) {
    /* Try git ls-files first — respects .gitignore */
    /* Escape single quotes in root to prevent shell injection:
     * replace each ' with '\'' (end quote, literal quote, start quote) */
    char escaped_root[NASH_PATH_MAX * 2];
    size_t ei = 0;
    for (size_t ri = 0; root[ri] && ei < sizeof(escaped_root) - 5; ri++) {
        if (root[ri] == '\'') {
            escaped_root[ei++] = '\'';
            escaped_root[ei++] = '\\';
            escaped_root[ei++] = '\'';
            escaped_root[ei++] = '\'';
        } else {
            escaped_root[ei++] = root[ri];
        }
    }
    escaped_root[ei] = '\0';
    char cmd[NASH_PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "git -C '%s' ls-files 2>/dev/null", escaped_root);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[NASH_PATH_MAX];
        int got_any = 0;
        while (fgets(line, sizeof(line), fp) && st->n_files < RM_MAX_FILES) {
            /* Strip newline */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
            if (len == 0) continue;

            if (!is_parseable(line) && !is_makefile(line)) continue;

            char fullpath[NASH_PATH_MAX];
            if (strcmp(root, ".") == 0)
                snprintf(fullpath, sizeof(fullpath), "%s", line);
            else
                snprintf(fullpath, sizeof(fullpath), "%s/%s", root, line);

            /* Verify it's a regular file */
            struct stat sb;
            if (stat(fullpath, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            /* Skip large files (>100KB) */
            if (sb.st_size > 100 * 1024) continue;

            snprintf(st->files[st->n_files].path, sizeof(st->files[0].path), "%s", line);
            st->files[st->n_files].lines = NULL;
            st->files[st->n_files].n_lines = 0;
            st->n_files++;
            got_any = 1;
        }
        pclose(fp);
        if (got_any) return;
    }

    /* Fallback: recursive directory walk */
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && st->n_files < RM_MAX_FILES) {
        if (ent->d_name[0] == '.') continue;

        char fullpath[NASH_PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, ent->d_name);

        struct stat sb;
        if (stat(fullpath, &sb) != 0) continue;

        if (S_ISDIR(sb.st_mode)) {
            if (!skip_dir(ent->d_name))
                discover_files(st, fullpath);
        } else if (S_ISREG(sb.st_mode)) {
            if (sb.st_size > 100 * 1024) continue;
            if (!is_parseable(fullpath) && !is_makefile(fullpath)) continue;

            /* Store relative path */
            const char *rel = fullpath;
            if (strncmp(rel, "./", 2) == 0) rel += 2;
            snprintf(st->files[st->n_files].path, sizeof(st->files[0].path), "%s", rel);
            st->files[st->n_files].lines = NULL;
            st->files[st->n_files].n_lines = 0;
            st->n_files++;
        }
    }
    closedir(d);
}

/* Load file lines into memory for later rendering */
static void load_file_lines(rm_file_t *f, const char *root) {
    char fullpath[NASH_PATH_MAX * 2];
    if (strcmp(root, ".") == 0)
        snprintf(fullpath, sizeof(fullpath), "%s", f->path);
    else
        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, f->path);

    FILE *fp = fopen(fullpath, "r");
    if (!fp) return;

    int cap = 256;
    f->lines = malloc(sizeof(char *) * (size_t)cap);
    if (!f->lines) { fclose(fp); return; }
    f->n_lines = 0;

    char buf[RM_MAX_LINE_LEN];
    while (fgets(buf, sizeof(buf), fp)) {
        if (f->n_lines >= cap) {
            cap *= 2;
            char **tmp = realloc(f->lines, sizeof(char *) * (size_t)cap);
            if (!tmp) break;
            f->lines = tmp;
        }
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        f->lines[f->n_lines] = strdup(buf);
        f->n_lines++;
    }
    fclose(fp);
}

static void free_file_lines(rm_file_t *f) {
    if (f->lines) {
        for (int i = 0; i < f->n_lines; i++)
            free(f->lines[i]);
        free(f->lines);
        f->lines = NULL;
    }
}

/* ── Phase 1: Tag Extraction ──────────────────────────────────────── */

/* Check if a character is valid in a C identifier */
static int is_ident_char(int c) {
    return isalnum(c) || c == '_';
}

/* Extract the identifier starting at s. Returns length. */
static int extract_ident(const char *s, char *out, int maxlen) {
    int i = 0;
    while (i < maxlen - 1 && is_ident_char((unsigned char)s[i])) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
    return i;
}

/* Add a tag (with dedup for definitions at same file+name) */
static void add_tag(rm_state_t *st, int file_idx, int line,
                    const char *name, tag_kind_t kind, sym_type_t sym_type) {
    if (st->n_tags >= RM_MAX_TAGS) return;
    if (!name[0]) return;

    /* Skip very short identifiers (single char) for references */
    if (kind == TAG_REFERENCE && strlen(name) < 2) return;

    /* For definitions, dedup by file+name */
    if (kind == TAG_DEFINITION) {
        for (int i = st->n_tags - 1; i >= 0 && i >= st->n_tags - 50; i--) {
            if (st->tags[i].file_idx == file_idx &&
                st->tags[i].kind == TAG_DEFINITION &&
                strcmp(st->tags[i].name, name) == 0)
                return;  /* already defined in this file */
        }
    }

    tag_t *t = &st->tags[st->n_tags++];
    t->file_idx = file_idx;
    t->line = line;
    strncpy(t->name, name, RM_MAX_IDENT_LEN - 1);
    t->name[RM_MAX_IDENT_LEN - 1] = '\0';
    t->kind = kind;
    t->sym_type = sym_type;
}

/* Is this line a C function definition? (non-indented, has parentheses,
 * preceded by a type, not a control-flow keyword) */
static int is_c_function_def(const char *line, char *fname, int maxlen) {
    /* Skip blank or preprocessor lines */
    if (line[0] == '\0' || line[0] == '#' || line[0] == ' ' || line[0] == '\t')
        return 0;
    if (line[0] == '/') return 0;  /* comment */
    if (line[0] == '{' || line[0] == '}') return 0;

    /* Must contain '(' */
    const char *paren = strchr(line, '(');
    if (!paren) return 0;

    /* The identifier before '(' */
    const char *p = paren - 1;
    while (p > line && (*p == ' ' || *p == '\t')) p--;
    if (p <= line) return 0;

    /* Walk back through identifier */
    const char *end = p + 1;
    while (p >= line && is_ident_char((unsigned char)*p)) p--;
    p++;

    if (p >= end) return 0;
    int len = (int)(end - p);
    if (len <= 0 || len >= maxlen) return 0;

    /* Skip control-flow keywords */
    static const char *keywords[] = {
        "if", "for", "while", "switch", "return", "sizeof", "typeof",
        "case", "else", "do", "goto", NULL
    };
    for (int i = 0; keywords[i]; i++) {
        if (len == (int)strlen(keywords[i]) && strncmp(p, keywords[i], (size_t)len) == 0)
            return 0;
    }

    /* Must have a return type before the function name */
    if (p == line) return 0;  /* no type before */

    memcpy(fname, p, (size_t)len);
    fname[len] = '\0';
    return 1;
}

/* Extract tags from a C/C++ file */
static void extract_c_tags(rm_state_t *st, int file_idx) {
    rm_file_t *f = &st->files[file_idx];
    if (!f->lines) return;

    int in_comment = 0;

    for (int i = 0; i < f->n_lines; i++) {
        const char *line = f->lines[i];
        int lineno = i + 1;

        /* Skip block comments */
        if (in_comment) {
            if (strstr(line, "*/")) in_comment = 0;
            continue;
        }
        if (strstr(line, "/*") && !strstr(line, "*/")) {
            in_comment = 1;
            continue;
        }

        /* Skip single-line comments */
        if (line[0] == '/' && line[1] == '/') continue;
        if (line[0] == '/' && line[1] == '*') continue;

        /* #include → reference */
        if (strncmp(line, "#include", 8) == 0) {
            const char *q = strchr(line, '"');
            if (q) {
                q++;
                char inc[RM_MAX_IDENT_LEN];
                int k = 0;
                while (*q && *q != '"' && k < RM_MAX_IDENT_LEN - 1)
                    inc[k++] = *q++;
                inc[k] = '\0';
                /* Extract base name without extension for matching */
                add_tag(st, file_idx, lineno, inc, TAG_REFERENCE, SYM_INCLUDE);
            }
            continue;
        }

        /* #define NAME → definition */
        if (strncmp(line, "#define ", 8) == 0) {
            char name[RM_MAX_IDENT_LEN];
            int len = extract_ident(line + 8, name, RM_MAX_IDENT_LEN);
            if (len > 2) {  /* skip trivially short macros */
                add_tag(st, file_idx, lineno, name, TAG_DEFINITION, SYM_MACRO);
            }
            continue;
        }

        /* typedef ... name; */
        if (strncmp(line, "typedef ", 8) == 0) {
            /* Look for the closing name - it's the last identifier before ';' */
            const char *semi = strrchr(line, ';');
            if (semi) {
                const char *p = semi - 1;
                while (p > line && (*p == ' ' || *p == '\t')) p--;
                const char *end = p + 1;
                while (p >= line && is_ident_char((unsigned char)*p)) p--;
                p++;
                if (p < end) {
                    char name[RM_MAX_IDENT_LEN];
                    int len = (int)(end - p);
                    if (len > 0 && len < RM_MAX_IDENT_LEN) {
                        memcpy(name, p, (size_t)len);
                        name[len] = '\0';
                        add_tag(st, file_idx, lineno, name, TAG_DEFINITION, SYM_TYPE);
                    }
                }
            }
            /* Also handle multi-line typedef struct { ... } name_t; */
            if (strstr(line, "struct") || strstr(line, "enum") || strstr(line, "union")) {
                if (!semi) {
                    /* Look forward for the closing }...name; */
                    for (int j = i + 1; j < f->n_lines && j < i + 200; j++) {
                        const char *cl = f->lines[j];
                        if (cl[0] == '}') {
                            const char *s2 = strchr(cl, ';');
                            if (s2) {
                                const char *pp = s2 - 1;
                                while (pp > cl && (*pp == ' ' || *pp == '\t')) pp--;
                                const char *ee = pp + 1;
                                while (pp > cl && is_ident_char((unsigned char)*pp)) pp--;
                                if (!is_ident_char((unsigned char)*pp)) pp++;
                                if (pp < ee) {
                                    char name[RM_MAX_IDENT_LEN];
                                    int len = (int)(ee - pp);
                                    if (len > 0 && len < RM_MAX_IDENT_LEN) {
                                        memcpy(name, pp, (size_t)len);
                                        name[len] = '\0';
                                        if (strcmp(name, "}") != 0)
                                            add_tag(st, file_idx, lineno, name, TAG_DEFINITION, SYM_TYPE);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
            continue;
        }

        /* struct/enum forward declarations or definitions */
        if ((strncmp(line, "struct ", 7) == 0 || strncmp(line, "enum ", 5) == 0) &&
            !strchr(line, '(')) {
            const char *start = line + (line[0] == 's' ? 7 : 5);
            char name[RM_MAX_IDENT_LEN];
            int len = extract_ident(start, name, RM_MAX_IDENT_LEN);
            if (len > 1) {
                add_tag(st, file_idx, lineno, name, TAG_DEFINITION, SYM_TYPE);
            }
            continue;
        }

        /* Function definitions (non-indented lines with parens) */
        {
            char fname[RM_MAX_IDENT_LEN];
            if (is_c_function_def(line, fname, RM_MAX_IDENT_LEN)) {
                add_tag(st, file_idx, lineno, fname, TAG_DEFINITION, SYM_FUNCTION);
            }
        }

        /* Function declarations in headers (extern or ending with ;).
         * Must be non-indented to avoid matching statements inside
         * function bodies (e.g. "if (x) return 1;"). */
        if (line[0] != ' ' && line[0] != '\t' && line[0] != '/'
            && strchr(line, '(') && strchr(line, ';')) {
            char fname[RM_MAX_IDENT_LEN];
            /* Try extracting function name before first '(' */
            const char *paren = strchr(line, '(');
            if (paren) {
                const char *p = paren - 1;
                while (p > line && (*p == ' ' || *p == '\t')) p--;
                const char *end = p + 1;
                while (p >= line && is_ident_char((unsigned char)*p)) p--;
                p++;
                int len = (int)(end - p);
                if (len > 1 && len < RM_MAX_IDENT_LEN) {
                    memcpy(fname, p, (size_t)len);
                    fname[len] = '\0';
                    /* Filter control-flow keywords */
                    static const char *kw[] = {
                        "if", "for", "while", "switch", "return",
                        "sizeof", "typeof", "case", "else", "do",
                        "goto", NULL
                    };
                    int is_kw = 0;
                    for (int k = 0; kw[k]; k++) {
                        if (len == (int)strlen(kw[k])
                            && strncmp(fname, kw[k], (size_t)len) == 0) {
                            is_kw = 1; break;
                        }
                    }
                    /* Only if it looks like a declaration (has type before) */
                    if (!is_kw && p > line)
                        add_tag(st, file_idx, lineno, fname, TAG_DEFINITION, SYM_FUNCTION);
                }
            }
        }
    }
}

/* Extract identifier references from a file by scanning for known definition names.
 * Called after all definitions are extracted. */
static void extract_references(rm_state_t *st, int file_idx) {
    rm_file_t *f = &st->files[file_idx];
    if (!f->lines) return;

    for (int i = 0; i < f->n_lines; i++) {
        const char *line = f->lines[i];
        int lineno = i + 1;

        /* Skip preprocessor lines (handled separately) */
        if (line[0] == '#') continue;

        /* Scan for identifiers and check if they match any definition */
        const char *p = line;
        while (*p) {
            if (is_ident_char((unsigned char)*p)) {
                char ident[RM_MAX_IDENT_LEN];
                int len = extract_ident(p, ident, RM_MAX_IDENT_LEN);

                /* Check if this identifier matches any definition from another file */
                if (len >= 3) {  /* skip trivial identifiers */
                    for (int t = 0; t < st->n_tags; t++) {
                        if (st->tags[t].kind == TAG_DEFINITION &&
                            st->tags[t].file_idx != file_idx &&
                            strcmp(st->tags[t].name, ident) == 0) {
                            add_tag(st, file_idx, lineno, ident, TAG_REFERENCE, st->tags[t].sym_type);
                            break;  /* one ref per occurrence is enough */
                        }
                    }
                }
                p += len;
            } else {
                p++;
            }
        }
    }
}

/* ── Phase 2: Graph Building ──────────────────────────────────────── */

typedef struct {
    int   from_file;   /* referencing file index */
    int   to_file;     /* defining file index */
    float weight;      /* edge weight */
} rm_edge_t;

typedef struct {
    rm_edge_t edges[RM_MAX_EDGES];
    int       n_edges;
    float     node_rank[RM_MAX_FILES];  /* PageRank score per file */
} rm_graph_t;

/* Check if an identifier is "meaningful" (snake_case/camelCase, >=6 chars) */
static int is_meaningful_ident(const char *name) {
    int len = (int)strlen(name);
    if (len < 6) return 0;
    /* Has underscore (snake_case) or mixed case (camelCase) */
    int has_sep = 0;
    for (int i = 0; i < len; i++) {
        if (name[i] == '_') { has_sep = 1; break; }
        if (i > 0 && isupper((unsigned char)name[i]) &&
            islower((unsigned char)name[i - 1])) { has_sep = 1; break; }
    }
    return has_sep;
}

/* Check if identifier starts with underscore (private) */
static int is_private_ident(const char *name) {
    return name[0] == '_';
}

/* Count how many files define this symbol */
static int count_defining_files(rm_state_t *st, const char *name) {
    int count = 0;
    int last_file = -1;
    for (int i = 0; i < st->n_tags; i++) {
        if (st->tags[i].kind == TAG_DEFINITION &&
            strcmp(st->tags[i].name, name) == 0 &&
            st->tags[i].file_idx != last_file) {
            count++;
            last_file = st->tags[i].file_idx;
        }
    }
    return count;
}

/* Check if a file is in the chat_files list */
static int is_chat_file(const char *path, const char **chat_files, int n_chat) {
    for (int i = 0; i < n_chat; i++) {
        if (strcmp(path, chat_files[i]) == 0) return 1;
        /* Also match basename */
        const char *base = strrchr(chat_files[i], '/');
        base = base ? base + 1 : chat_files[i];
        const char *pbase = strrchr(path, '/');
        pbase = pbase ? pbase + 1 : path;
        if (strcmp(pbase, base) == 0) return 1;
    }
    return 0;
}

/* Check if user_query mentions this identifier */
static int query_mentions(const char *query, const char *name) {
    if (!query || !name) return 0;
    return strstr(query, name) != NULL;
}

/* Add or update an edge (aggregate weight) */
static void add_edge(rm_graph_t *g, int from, int to, float weight) {
    if (from == to) return;  /* no self-edges */

    /* Check for existing edge and aggregate */
    for (int i = 0; i < g->n_edges; i++) {
        if (g->edges[i].from_file == from && g->edges[i].to_file == to) {
            g->edges[i].weight += weight;
            return;
        }
    }

    if (g->n_edges >= RM_MAX_EDGES) return;
    rm_edge_t *e = &g->edges[g->n_edges++];
    e->from_file = from;
    e->to_file = to;
    e->weight = weight;
}

static void build_graph(rm_state_t *st, rm_graph_t *g,
                        const char *user_query,
                        const char **chat_files, int n_chat) {
    memset(g, 0, sizeof(*g));

    /* For each reference tag, find the corresponding definition tag and add edge */
    for (int i = 0; i < st->n_tags; i++) {
        tag_t *ref = &st->tags[i];
        if (ref->kind != TAG_REFERENCE) continue;

        /* Find definition(s) of this symbol */
        for (int j = 0; j < st->n_tags; j++) {
            tag_t *def = &st->tags[j];
            if (def->kind != TAG_DEFINITION) continue;
            if (def->file_idx == ref->file_idx) continue;

            /* Match by name, or for includes by file path */
            int match = 0;
            if (ref->sym_type == SYM_INCLUDE) {
                /* #include "foo.h" matches foo.h */
                const char *base = strrchr(st->files[def->file_idx].path, '/');
                base = base ? base + 1 : st->files[def->file_idx].path;
                if (strcmp(ref->name, base) == 0 ||
                    strcmp(ref->name, st->files[def->file_idx].path) == 0) {
                    match = 1;
                }
            } else {
                match = (strcmp(ref->name, def->name) == 0);
            }

            if (!match) continue;

            /* Calculate edge weight with multipliers */
            float weight = 1.0f;

            /* ×10 if mentioned in user query */
            if (query_mentions(user_query, ref->name))
                weight *= 10.0f;

            /* ×10 if meaningful identifier */
            if (is_meaningful_ident(ref->name))
                weight *= 10.0f;

            /* ×0.1 if private */
            if (is_private_ident(ref->name))
                weight *= 0.1f;

            /* ×0.1 if defined in >5 files (too common) */
            if (count_defining_files(st, ref->name) > 5)
                weight *= 0.1f;

            /* ×50 if reference is FROM a chat file */
            if (chat_files && n_chat > 0 &&
                is_chat_file(st->files[ref->file_idx].path, chat_files, n_chat))
                weight *= 50.0f;

            /* Dampening for multiple refs (sqrt) */
            /* (already aggregated in add_edge) */

            add_edge(g, ref->file_idx, def->file_idx, weight);
        }
    }
}

/* ── Phase 3: PageRank ────────────────────────────────────────────── */

static void pagerank(rm_state_t *st, rm_graph_t *g,
                     const char **chat_files, int n_chat) {
    int n = st->n_files;
    if (n == 0) return;

    /* Initialize personalization vector */
    float *pers = calloc((size_t)n, sizeof(float));
    float *rank = calloc((size_t)n, sizeof(float));
    float *new_rank = calloc((size_t)n, sizeof(float));
    float *out_weight = calloc((size_t)n, sizeof(float));
    if (!pers || !rank || !new_rank || !out_weight) {
        free(pers); free(rank); free(new_rank); free(out_weight);
        return;
    }

    /* Personalization: bias toward chat files */
    int n_pers = 0;
    if (chat_files && n_chat > 0) {
        for (int i = 0; i < n; i++) {
            if (is_chat_file(st->files[i].path, chat_files, n_chat)) {
                pers[i] = 1.0f;
                n_pers++;
            }
        }
    }
    if (n_pers == 0) {
        /* No chat files — uniform personalization */
        for (int i = 0; i < n; i++)
            pers[i] = 1.0f / (float)n;
    } else {
        for (int i = 0; i < n; i++)
            pers[i] /= (float)n_pers;
    }

    /* Initialize rank uniformly */
    for (int i = 0; i < n; i++)
        rank[i] = 1.0f / (float)n;

    /* Compute total outgoing weight per node */
    for (int e = 0; e < g->n_edges; e++)
        out_weight[g->edges[e].from_file] += g->edges[e].weight;

    /* Power iteration */
    for (int iter = 0; iter < RM_PAGERANK_ITERS; iter++) {
        /* Reset new_rank to teleport component */
        float dangling = 0.0f;
        for (int i = 0; i < n; i++) {
            if (out_weight[i] == 0.0f)
                dangling += rank[i];
        }

        for (int i = 0; i < n; i++)
            new_rank[i] = (1.0f - RM_PAGERANK_DAMP) * pers[i] +
                          RM_PAGERANK_DAMP * dangling * pers[i];

        /* Distribute rank along edges */
        for (int e = 0; e < g->n_edges; e++) {
            int from = g->edges[e].from_file;
            int to = g->edges[e].to_file;
            if (out_weight[from] > 0.0f) {
                float contrib = RM_PAGERANK_DAMP * rank[from] *
                               (g->edges[e].weight / out_weight[from]);
                new_rank[to] += contrib;
            }
        }

        /* Normalize */
        float total = 0.0f;
        for (int i = 0; i < n; i++) total += new_rank[i];
        if (total > 0.0f) {
            for (int i = 0; i < n; i++) new_rank[i] /= total;
        }

        /* Swap */
        float *tmp = rank;
        rank = new_rank;
        new_rank = tmp;
    }

    /* Store results */
    for (int i = 0; i < n; i++)
        g->node_rank[i] = rank[i];

    free(pers);
    free(rank);
    free(new_rank);
    free(out_weight);
}

/* ── Phase 4: Rank Distribution to Symbols ────────────────────────── */

typedef struct {
    int    file_idx;
    int    tag_idx;     /* index into tags array (for the definition) */
    float  score;
} ranked_sym_t;

/* Compare for descending sort */
static int cmp_ranked_sym(const void *a, const void *b) {
    float da = ((const ranked_sym_t *)a)->score;
    float db = ((const ranked_sym_t *)b)->score;
    if (db > da) return 1;
    if (db < da) return -1;
    return 0;
}

/* Distribute file-level PageRank to individual definitions.
 * Each definition's score = file_rank * (incoming_edge_weight_for_this_symbol / total_file_edges).
 * Definitions with no incoming references get a small base score. */
static int distribute_rank(rm_state_t *st, rm_graph_t *g,
                           ranked_sym_t *ranked, int max_ranked,
                           const char **chat_files, int n_chat) {
    int n_ranked = 0;

    for (int fi = 0; fi < st->n_files && n_ranked < max_ranked; fi++) {
        float file_rank = g->node_rank[fi];
        if (file_rank <= 0.0f) continue;

        /* Skip files already in chat */
        if (chat_files && n_chat > 0 &&
            is_chat_file(st->files[fi].path, chat_files, n_chat))
            continue;

        /* Count definitions in this file */
        int n_defs = 0;
        for (int t = 0; t < st->n_tags; t++) {
            if (st->tags[t].file_idx == fi && st->tags[t].kind == TAG_DEFINITION)
                n_defs++;
        }
        if (n_defs == 0) continue;

        /* Count total incoming edge weight for this file */
        float total_in = 0.0f;
        for (int e = 0; e < g->n_edges; e++) {
            if (g->edges[e].to_file == fi)
                total_in += g->edges[e].weight;
        }

        /* Distribute rank to each definition */
        /* TODO(perf): O(n^4) complexity — files × tags × edges × tags.
         * Build a hash map from (file_idx, symbol_name) -> edge weight sum
         * to reduce the inner two loops from O(edges × tags) to O(edges + tags). */
        for (int t = 0; t < st->n_tags && n_ranked < max_ranked; t++) {
            if (st->tags[t].file_idx != fi || st->tags[t].kind != TAG_DEFINITION)
                continue;

            float sym_weight = 0.0f;
            /* Sum incoming edges that reference this specific symbol */
            for (int e = 0; e < g->n_edges; e++) {
                if (g->edges[e].to_file != fi) continue;
                /* Check if the referencing file has a reference to this symbol */
                int ref_file = g->edges[e].from_file;
                for (int rt = 0; rt < st->n_tags; rt++) {
                    if (st->tags[rt].file_idx == ref_file &&
                        st->tags[rt].kind == TAG_REFERENCE &&
                        strcmp(st->tags[rt].name, st->tags[t].name) == 0) {
                        sym_weight += g->edges[e].weight;
                        break;
                    }
                }
            }

            float score;
            if (total_in > 0.0f && sym_weight > 0.0f)
                score = file_rank * (sym_weight / total_in);
            else
                score = file_rank / (float)n_defs;  /* equal share */

            ranked[n_ranked].file_idx = fi;
            ranked[n_ranked].tag_idx = t;
            ranked[n_ranked].score = score;
            n_ranked++;
        }
    }

    /* Sort by score descending */
    qsort(ranked, (size_t)n_ranked, sizeof(ranked_sym_t), cmp_ranked_sym);
    return n_ranked;
}

/* ── Phase 5 & 6: Budget Fitting + Scope-Aware Rendering ──────────── */

/* Render a file's relevant definitions with ⋮ elision.
 * Shows definition lines + parent scope (preceding { lines),
 * elides everything else with ⋮ markers. */
static void render_file(str_t *out, rm_state_t *st, int file_idx,
                        const int *show_lines, int n_show, const char *root) {
    rm_file_t *f = &st->files[file_idx];

    /* Load lines if not already loaded */
    if (!f->lines)
        load_file_lines(f, root);
    if (!f->lines || f->n_lines == 0) return;

    /* Build a set of lines to show: the definition lines + context */
    int *visible = calloc((size_t)f->n_lines, sizeof(int));
    if (!visible) return;

    for (int s = 0; s < n_show; s++) {
        int line0 = show_lines[s] - 1;  /* 0-based */
        if (line0 < 0 || line0 >= f->n_lines) continue;

        /* Show the definition line itself */
        visible[line0] = 1;

        /* Show parent scope headers: walk up to find containing { */
        int brace_depth = 0;
        for (int j = line0 - 1; j >= 0; j--) {
            const char *l = f->lines[j];
            /* Count braces */
            for (const char *c = l; *c; c++) {
                if (*c == '}') brace_depth++;
                else if (*c == '{') brace_depth--;
            }
            if (brace_depth < 0) {
                /* This line opens a scope — show it */
                visible[j] = 1;
                /* Also show the line(s) before it if they're a function/struct signature */
                for (int k = j - 1; k >= 0 && k >= j - 3; k--) {
                    const char *pl = f->lines[k];
                    if (pl[0] && pl[0] != '{' && pl[0] != '}' &&
                        pl[0] != '\n' && pl[0] != '\0') {
                        visible[k] = 1;
                    } else {
                        break;
                    }
                }
                brace_depth = 0;  /* reset for next outer scope */
            }
        }

        /* Show a few lines of context after the definition */
        for (int j = line0 + 1; j <= line0 + 2 && j < f->n_lines; j++) {
            const char *l = f->lines[j];
            /* Show struct/enum member lines, function params continuation */
            if (l[0] == ' ' || l[0] == '\t') {
                visible[j] = 1;
            } else {
                break;
            }
        }
    }

    /* Render with ⋮ for gaps */
    str_appendf(out, "%s:\n", f->path);
    int last_shown = -2;  /* -2 = nothing shown yet */
    for (int i = 0; i < f->n_lines; i++) {
        if (visible[i]) {
            if (last_shown >= 0 && i > last_shown + 1) {
                str_appendf(out, "⋮\n");
            } else if (last_shown == -2 && i > 0) {
                str_appendf(out, "⋮\n");
            }
            /* Render the line with leading │ */
            str_appendf(out, "│%s\n", f->lines[i]);
            last_shown = i;
        }
    }
    if (last_shown >= 0 && last_shown < f->n_lines - 1) {
        str_appendf(out, "⋮\n");
    }
    str_appendf(out, "\n");

    free(visible);
}

/* ── Main Entry Point ─────────────────────────────────────────────── */

char *repomap_build(const char *root_dir, const char *user_query,
                    const char **chat_files, int n_chat_files,
                    int max_chars) {
    if (max_chars <= 0) max_chars = RM_DEFAULT_CHARS;
    const char *root = root_dir ? root_dir : ".";

    /* Allocate state */
    rm_state_t *st = calloc(1, sizeof(rm_state_t));
    if (!st) return NULL;

    /* Phase 1a: Discover files */
    discover_files(st, root);
    if (st->n_files == 0) {
        free(st);
        return NULL;
    }

    /* Phase 1b: Load and parse files for definitions */
    for (int i = 0; i < st->n_files; i++) {
        load_file_lines(&st->files[i], root);
        extract_c_tags(st, i);
    }

    /* Phase 1c: Extract references (need all definitions first) */
    for (int i = 0; i < st->n_files; i++) {
        extract_references(st, i);
    }

    /* Phase 2: Build graph */
    rm_graph_t *g = calloc(1, sizeof(rm_graph_t));
    if (!g) {
        for (int i = 0; i < st->n_files; i++) free_file_lines(&st->files[i]);
        free(st);
        return NULL;
    }
    build_graph(st, g, user_query, chat_files, n_chat_files);

    /* Phase 3: PageRank */
    pagerank(st, g, chat_files, n_chat_files);

    /* Phase 4: Distribute rank to symbols */
    int max_ranked = st->n_tags < 4096 ? st->n_tags : 4096;
    ranked_sym_t *ranked = malloc(sizeof(ranked_sym_t) * (size_t)max_ranked);
    int n_ranked = 0;
    if (ranked) {
        n_ranked = distribute_rank(st, g, ranked, max_ranked,
                                   chat_files, n_chat_files);
    }

    /* Phase 5 & 6: Budget fitting + rendering */
    str_t out = str_new(4096);
    str_appendf(&out, "[REPO MAP]\n"
        "Summaries of relevant source files. "
        "Treat as read-only structural context.\n\n");

    /* Group ranked symbols by file, take top symbols per file */
    int chars_used = (int)out.len;

    /* Track which files we've rendered and their show-lines */
    typedef struct {
        int file_idx;
        int show_lines[64];
        int n_show;
    } file_render_t;

    int fr_cap = st->n_files < 64 ? st->n_files : 64;
    file_render_t *frs = calloc((size_t)fr_cap, sizeof(file_render_t));
    int n_frs = 0;

    if (frs && ranked) {
        for (int r = 0; r < n_ranked && chars_used < max_chars; r++) {
            int fi = ranked[r].file_idx;
            int tag_i = ranked[r].tag_idx;
            int def_line = st->tags[tag_i].line;

            /* Find or create file_render entry */
            int fri = -1;
            for (int k = 0; k < n_frs; k++) {
                if (frs[k].file_idx == fi) { fri = k; break; }
            }
            if (fri < 0) {
                if (n_frs >= fr_cap) break;
                fri = n_frs++;
                frs[fri].file_idx = fi;
                frs[fri].n_show = 0;
            }

            /* Add this definition's line */
            if (frs[fri].n_show < 64) {
                /* Avoid duplicate lines */
                int dup = 0;
                for (int k = 0; k < frs[fri].n_show; k++) {
                    if (frs[fri].show_lines[k] == def_line) { dup = 1; break; }
                }
                if (!dup) {
                    frs[fri].show_lines[frs[fri].n_show++] = def_line;
                    /* Rough estimate: ~80 chars per shown line + context */
                    chars_used += 120;
                }
            }
        }

        /* Render each file */
        for (int f = 0; f < n_frs; f++) {
            size_t before = out.len;
            render_file(&out, st, frs[f].file_idx, frs[f].show_lines,
                       frs[f].n_show, root);

            /* Check budget */
            if ((int)out.len > max_chars) {
                /* Truncate to last complete file */
                out.data[before] = '\0';
                out.len = before;
                break;
            }
        }
    }

    /* Cleanup */
    free(frs);
    free(ranked);
    free(g);
    for (int i = 0; i < st->n_files; i++)
        free_file_lines(&st->files[i]);
    free(st);

    if (out.len <= 80) {
        /* Only header, no actual content */
        str_free(&out);
        return NULL;
    }

    return str_steal(&out);
}

/* ── Per-file Symbol Extraction (for eviction breadcrumbs) ─────────── */

/* Split in-memory content into a lines array (replaces load_file_lines I/O).
 * Caller must free each line and the array itself. */
static char **split_content_lines(const char *content, int content_len,
                                  int *out_n_lines) {
    *out_n_lines = 0;
    if (!content || content_len <= 0) return NULL;

    int cap = 128;
    char **lines = malloc(sizeof(char *) * (size_t)cap);
    if (!lines) return NULL;

    const char *p = content;
    const char *end = content + content_len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        int len = (int)(line_end - p);

        /* Clamp to RM_MAX_LINE_LEN to match load_file_lines behavior */
        if (len >= RM_MAX_LINE_LEN) len = RM_MAX_LINE_LEN - 1;

        if (*out_n_lines >= cap) {
            cap *= 2;
            char **tmp = realloc(lines, sizeof(char *) * (size_t)cap);
            if (!tmp) break;
            lines = tmp;
        }

        lines[*out_n_lines] = malloc((size_t)(len + 1));
        if (!lines[*out_n_lines]) break;
        memcpy(lines[*out_n_lines], p, (size_t)len);
        lines[*out_n_lines][len] = '\0';
        (*out_n_lines)++;

        p = nl ? nl + 1 : end;
    }

    return lines;
}

/* Suffix decoration for symbol type: "()" for functions, nothing for others. */
static const char *sym_suffix(sym_type_t st) {
    return (st == SYM_FUNCTION) ? "()" : "";
}

int repomap_file_symbols(const char *content, int content_len,
                         const char *filename, char *out, int out_cap) {
    if (!content || content_len <= 0 || !filename || !out || out_cap < 2) {
        if (out && out_cap > 0) out[0] = '\0';
        return 0;
    }
    out[0] = '\0';

    /* Language detection via extension — only C/C++ has an extractor */
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    int is_c = (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
                strcmp(dot, ".cpp") == 0 || strcmp(dot, ".hpp") == 0 ||
                strcmp(dot, ".cc") == 0 || strcmp(dot, ".hh") == 0);
    if (!is_c) return 0;

    /* Split content into lines */
    int n_lines = 0;
    char **lines = split_content_lines(content, content_len, &n_lines);
    if (!lines || n_lines == 0) {
        free(lines);
        return 0;
    }

    /* Build minimal rm_state_t with one file entry.
     * Heap-allocated because rm_state_t is ~3.3MB (files[512] with
     * NASH_PATH_MAX paths + tags[8192]), far too large for the stack. */
    rm_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        for (int i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return 0;
    }
    st->n_files = 1;
    st->files[0].lines = lines;
    st->files[0].n_lines = n_lines;
    /* path is only used for graph building, not needed here */
    st->files[0].path[0] = '\0';

    /* Run Phase 1 C tag extraction */
    extract_c_tags(st, 0);

    /* Collect TAG_DEFINITION entries into output buffer */
    int written = 0;
    int n_syms = 0;
    for (int i = 0; i < st->n_tags; i++) {
        if (st->tags[i].kind != TAG_DEFINITION) continue;
        if (st->tags[i].sym_type == SYM_INCLUDE) continue;  /* skip #include refs */

        const char *name = st->tags[i].name;
        const char *suf = sym_suffix(st->tags[i].sym_type);
        int name_len = (int)strlen(name);
        int suf_len = (int)strlen(suf);
        int sep_len = (n_syms > 0) ? 2 : 0;  /* ", " separator */
        int need = sep_len + name_len + suf_len;

        if (written + need >= out_cap - 1) break;  /* no room */

        if (n_syms > 0) {
            out[written++] = ',';
            out[written++] = ' ';
        }
        memcpy(out + written, name, (size_t)name_len);
        written += name_len;
        memcpy(out + written, suf, (size_t)suf_len);
        written += suf_len;
        n_syms++;
    }
    out[written] = '\0';

    /* Cleanup */
    for (int i = 0; i < n_lines; i++) free(lines[i]);
    free(lines);
    free(st);

    return written;
}
