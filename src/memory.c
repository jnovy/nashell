#define _GNU_SOURCE
#include "memory.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <strings.h>  /* strcasestr */
#include <unistd.h>   /* unlink, fork, execvp, dup2, chdir, _exit */
#include <sys/wait.h> /* waitpid */
#include <fcntl.h>    /* open, O_WRONLY */
#include <math.h>     /* exp, log */

/* ── helpers ─────────────────────────────────────────── */

/* Sanitize key for filename: replace : with _ */
static void key_to_filename(const char *key, char *out, size_t out_sz) {
    size_t i = 0;
    for (; key[i] && i < out_sz - 6; i++)
        out[i] = (key[i] == ':' || key[i] == '/') ? '_' : key[i];
    out[i] = '\0';
    strcat(out, ".json");
}

static double epoch_now(void) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    return (double)tp.tv_sec + (double)tp.tv_nsec / 1e9;
}

/* ── git version control for memory store ──────────────────────── */

/* Run a git command in the memory directory. Returns 0 on success. */
static int memory_git_run(memory_t *m, const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: chdir to memory dir, suppress output */
        if (chdir(m->dir) != 0) _exit(1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Initialize git repo in .memory/ if not already initialized.
 * Called on first memory_store — lazy init. */
static void memory_git_init(memory_t *m) {
    if (!m) return;
    char git_path[4096];
    snprintf(git_path, sizeof(git_path), "%s/.git", m->dir);
    struct stat st;
    if (stat(git_path, &st) == 0) return;  /* already initialized */

    const char *init_argv[] = {"git", "init", "-q", NULL};
    if (memory_git_run(m, init_argv) != 0) return;

    /* Configure user for commits (required by git) */
    const char *name_argv[] = {"git", "config", "user.name", "nash", NULL};
    memory_git_run(m, name_argv);
    const char *email_argv[] = {"git", "config", "user.email", "nash@localhost", NULL};
    memory_git_run(m, email_argv);

    /* Initial commit with any existing files */
    const char *add_argv[] = {"git", "add", "-A", NULL};
    memory_git_run(m, add_argv);
    const char *commit_argv[] = {"git", "commit", "-q", "--allow-empty",
                                  "-m", "memory: initialize memory store", NULL};
    memory_git_run(m, commit_argv);
}

/* Stage all changes and commit with a descriptive message.
 * Appends "Stored-by: <model>" signoff when model is known.
 * No-op if nothing changed (git commit will exit 1, which we ignore). */
static void memory_git_commit(memory_t *m, const char *msg) {
    if (!m || !msg) return;
    char git_path[4096];
    snprintf(git_path, sizeof(git_path), "%s/.git", m->dir);
    struct stat st;
    if (stat(git_path, &st) != 0) return;  /* no git repo */

    const char *add_argv[] = {"git", "add", "-A", NULL};
    memory_git_run(m, add_argv);

    /* Append model signoff if available (like /dream's Consolidated-by:) */
    char full_msg[1024];
    if (m->model) {
        snprintf(full_msg, sizeof(full_msg), "%s\n\nStored-by: %s", msg, m->model);
    } else {
        snprintf(full_msg, sizeof(full_msg), "%s", msg);
    }

    const char *commit_argv[] = {"git", "commit", "-q", "--allow-empty-message",
                                  "-m", full_msg, NULL};
    memory_git_run(m, commit_argv);
}

/* ── create/free ─────────────────────────────────────── */

memory_t *memory_new(const char *project_root) {
    memory_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    char path[4096];
    snprintf(path, sizeof(path), "%s/memory", project_root);
    mkdir(path, 0755);
    m->dir = strdup(path);
    return m;
}

void memory_free(memory_t *m) {
    if (!m) return;
    embed_free(m->embed);
    free(m->dir);
    free(m->model);
    free(m);
}

/* ── embedding helpers ───────────────────────────────── */

/* Convert a .json path to .emb path for embedding storage */
static void json_to_emb_path(const char *json_path, char *emb_path, size_t sz) {
    snprintf(emb_path, sz, "%s", json_path);
    size_t len = strlen(emb_path);
    if (len >= 5 && strcmp(emb_path + len - 5, ".json") == 0) {
        strcpy(emb_path + len - 5, ".emb");
    }
}

/* Convert a key to .emb filename */
static void key_to_emb_filename(const char *key, char *out, size_t out_sz) {
    size_t i = 0;
    for (; key[i] && i < out_sz - 5; i++)
        out[i] = (key[i] == ':' || key[i] == '/') ? '_' : key[i];
    out[i] = '\0';
    strcat(out, ".emb");
}

/* ── store ───────────────────────────────────────────── */

int memory_store(memory_t *m, const char *key, const char *value,
                 const char **tags, int n_tags, int pinned,
                 const char *journal_ref) {
    if (!m || !key || !value) return -1;

    /* Initialize git repo on first store */
    memory_git_init(m);

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "key", key);
    cJSON_AddStringToObject(entry, "value", value);

    cJSON *tags_arr = cJSON_CreateArray();
    for (int i = 0; i < n_tags; i++)
        cJSON_AddItemToArray(tags_arr, cJSON_CreateString(tags[i]));
    cJSON_AddItemToObject(entry, "tags", tags_arr);

    cJSON_AddBoolToObject(entry, "pinned", pinned);

    /* Check if entry already exists (preserve counters) */
    FILE *existing = fopen(path, "r");
    int access_count = 1;  /* store itself counts as one access */
    int recall_hits = 0;
    int recall_misses = 0;
    double created_at = epoch_now();
    if (existing) {
        fseek(existing, 0, SEEK_END);
        long sz = ftell(existing);
        fseek(existing, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (buf) {
            fread(buf, 1, (size_t)sz, existing);
            buf[sz] = '\0';
            cJSON *old = cJSON_Parse(buf);
            if (old) {
                cJSON *ac = cJSON_GetObjectItem(old, "access_count");
                if (ac) access_count = (int)cJSON_GetNumberValue(ac) + 1;  /* increment */
                cJSON *ca = cJSON_GetObjectItem(old, "created_at");
                if (ca && ca->valuestring) created_at = atof(ca->valuestring);
                else if (ca) created_at = cJSON_GetNumberValue(ca);
                cJSON *rh = cJSON_GetObjectItem(old, "recall_hits");
                if (rh) recall_hits = (int)cJSON_GetNumberValue(rh);
                cJSON *rm = cJSON_GetObjectItem(old, "recall_misses");
                if (rm) recall_misses = (int)cJSON_GetNumberValue(rm);
                cJSON_Delete(old);
            }
            free(buf);
        }
        fclose(existing);
    }

    char ts[32];
    snprintf(ts, sizeof(ts), "%.5f", created_at);
    cJSON_AddStringToObject(entry, "created_at", ts);
    snprintf(ts, sizeof(ts), "%.5f", epoch_now());
    cJSON_AddStringToObject(entry, "last_accessed", ts);
    cJSON_AddNumberToObject(entry, "access_count", access_count);
    cJSON_AddNumberToObject(entry, "recall_hits", recall_hits);
    cJSON_AddNumberToObject(entry, "recall_misses", recall_misses);

    /* Provenance: link to the session journal where this memory was created.
     * The dreaming LLM can read this journal to understand original context. */
    if (journal_ref)
        cJSON_AddStringToObject(entry, "journal_ref", journal_ref);

    char *json = cJSON_Print(entry);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);

    /* Generate embedding for semantic matching (if enabled) */
    if (m->embed && m->embed->available) {
        memory_embed_entry(m, key, value, tags, n_tags);
    }

    /* Auto-update MEMORY.md index file */
    memory_write_index_file(m);

    /* Git commit: track memory creation/update */
    char commit_msg[256];
    snprintf(commit_msg, sizeof(commit_msg), "memory: store %s", key);
    memory_git_commit(m, commit_msg);

    return 0;
}


/* ── pin/unpin ───────────────────────────────────────────── */

/* Helper: load entry JSON from file, modify pinned flag, write back */
static int memory_set_pinned(memory_t *m, const char *key, int pinned) {
    if (!m || !key) return -1;

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* entry not found */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *entry = cJSON_Parse(buf);
    free(buf);
    if (!entry) return -1;

    /* Replace or add the pinned field */
    cJSON *p = cJSON_GetObjectItem(entry, "pinned");
    if (p) {
        cJSON_ReplaceItemInObject(entry, "pinned",
                                  pinned ? cJSON_CreateTrue() : cJSON_CreateFalse());
    } else {
        cJSON_AddBoolToObject(entry, "pinned", pinned);
    }

    /* Write back */
    char *json = cJSON_Print(entry);
    f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);

    /* Update MEMORY.md index */
    memory_write_index_file(m);

    /* Git: commit pin/unpin change */
    char msg[600];
    snprintf(msg, sizeof(msg), "memory: %s %s",
             pinned ? "pin" : "unpin", key);
    memory_git_commit(m, msg);

    return 0;
}

int memory_pin(memory_t *m, const char *key) {
    return memory_set_pinned(m, key, 1);
}

int memory_unpin(memory_t *m, const char *key) {
    return memory_set_pinned(m, key, 0);
}

/* ── recall (search) ─────────────────────────────────── */

/* Substring-based relevance scoring (fallback when embeddings unavailable) */
static double score_entry_substring(const char *key, const char *value,
                                     cJSON *tags, const char *query) {
    double relevance = 0;
    if (strcasestr(key, query)) relevance += 3.0;
    if (tags) {
        int n = cJSON_GetArraySize(tags);
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetArrayItem(tags, i);
            if (t && t->valuestring && strcasestr(t->valuestring, query))
                relevance += 2.0;
        }
    }
    if (strcasestr(value, query)) relevance += 1.0;
    return relevance;
}

/* Composite scoring: semantic + substring + recency + importance.
 * When embeddings are available, semantic similarity is the primary signal.
 * Substring matching provides a safety net for exact keyword matches that
 * embeddings might underweight (e.g., function names, error codes).
 *
 * Inspired by GDN-2's "short convolution on gates": instead of scoring
 * each memory independently via substring, we use dense vector embeddings
 * that capture the full semantic context of the query — a continuous,
 * context-aware relevance signal. */
static double score_entry_hybrid(const char *key, const char *value,
                                  cJSON *tags, const char *query,
                                  double last_accessed, int access_count,
                                  float semantic_sim, int has_semantic) {
    double relevance;

    if (has_semantic) {
        /* Semantic mode: cosine similarity is primary signal.
         * Scale from [-1,1] to [0,6] range to match substring score scale.
         * Add substring bonus for exact keyword matches. */
        double semantic = (double)(semantic_sim + 1.0f) * 3.0;  /* [0, 6] */
        double substring = score_entry_substring(key, value, tags, query);

        /* Blend: 70% semantic + 30% substring.
         * This ensures exact keyword matches still surface even if
         * the embedding model doesn't capture them well. */
        relevance = semantic * 0.7 + substring * 0.3;
    } else {
        /* Fallback: pure substring matching (original behavior) */
        relevance = score_entry_substring(key, value, tags, query);
        if (relevance == 0) return 0;  /* no match at all */
    }

    if (relevance < 0.01) return 0;

    /* Recency: exponential decay — recent memories score higher */
    double age_days = (epoch_now() - last_accessed) / 86400.0;
    if (age_days < 0) age_days = 0;
    double recency = exp(-age_days / 30.0);  /* half-life ~30 days */

    /* Importance: logarithmic access frequency */
    double importance = 1.0 + log(1.0 + (double)access_count);

    /* Composite: weighted combination */
    return relevance * 0.6 + recency * 0.2 + importance * 0.2;
}

/* qsort comparator for scored entries (descending by score) */
typedef struct { char path[4096]; double score; cJSON *cached_entry; } scored_t;

static int scored_cmp_desc(const void *a, const void *b) {
    double sa = ((const scored_t *)a)->score;
    double sb = ((const scored_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

memory_results_t memory_recall(memory_t *m, const char *query, int max_results) {
    memory_results_t results = {0};
    if (!m || !query) return results;

    DIR *dir = opendir(m->dir);
    if (!dir) return results;

    /* Generate query embedding once (if embeddings are available) */
    embed_vec_t query_emb = {0};
    int has_semantic = 0;
    if (m->embed && m->embed->available) {
        query_emb = embed_text(m->embed, query);
        has_semantic = (query_emb.data != NULL && query_emb.dim > 0);
    }

    /* Dynamic scored array — grows as needed (FIX #5: no fixed 1024 limit) */
    int scored_cap = 256;
    scored_t *scored = calloc((size_t)scored_cap, sizeof(scored_t));
    int n_scored = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        const char *key = "";
        const char *value = "";
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *v = cJSON_GetObjectItem(entry, "value");
        cJSON *tags = cJSON_GetObjectItem(entry, "tags");
        if (k && k->valuestring) key = k->valuestring;
        if (v && v->valuestring) value = v->valuestring;

        /* Get recency/importance data for composite scoring */
        double last_acc = 0;
        int acc_count = 0;
        cJSON *la = cJSON_GetObjectItem(entry, "last_accessed");
        cJSON *ac = cJSON_GetObjectItem(entry, "access_count");
        if (la && la->valuestring) last_acc = atof(la->valuestring);
        if (ac) acc_count = (int)cJSON_GetNumberValue(ac);

        /* Compute semantic similarity if embeddings available */
        float semantic_sim = 0.0f;
        int entry_has_semantic = 0;
        if (has_semantic) {
            /* Try to load cached embedding for this entry */
            char emb_path[4096];
            json_to_emb_path(path, emb_path, sizeof(emb_path));
            embed_vec_t entry_emb = embed_vec_load(emb_path);
            if (entry_emb.data && entry_emb.dim > 0) {
                /* FIX #9: Dimension check — skip stale embeddings from a
                 * different model (e.g., switched from MiniLM-384d to
                 * nomic-embed-768d). Mismatched dims would give 0.0 from
                 * cosine_sim anyway, but this makes the intent explicit. */
                if (entry_emb.dim == query_emb.dim) {
                    semantic_sim = embed_cosine_sim(&query_emb, &entry_emb);
                    entry_has_semantic = 1;
                } else {
                    /* Stale embedding — delete it so memory_embed_all can
                     * regenerate it with the current model on next startup */
                    unlink(emb_path);
                }
            }
            embed_vec_free(&entry_emb);
        }

        double s = score_entry_hybrid(key, value, tags, query,
                                       last_acc, acc_count,
                                       semantic_sim, entry_has_semantic);

        /* FIX #13: Type-aware filtering — if query starts with a type prefix
         * (e.g., "skill:", "lesson:", "strategy:"), only return entries of
         * that type. This replaces the previous hack of searching for "skill:"
         * as a substring query. */
        if (s > 0.01) {
            static const char *type_prefixes[] = {
                "skill:", "lesson:", "strategy:", "fact:", "task:", NULL
            };
            for (const char **pfx = type_prefixes; *pfx; pfx++) {
                size_t plen = strlen(*pfx);
                if (strncmp(query, *pfx, plen) == 0) {
                    /* Query has type prefix — only match entries of same type */
                    if (strncmp(key, *pfx, plen) != 0) {
                        s = 0;  /* wrong type — exclude */
                    }
                    break;
                }
            }
        }

        if (s > 0.01) {
            /* Grow scored array if needed (FIX #5) */
            if (n_scored >= scored_cap) {
                scored_cap *= 2;
                scored = realloc(scored, (size_t)scored_cap * sizeof(scored_t));
            }
            snprintf(scored[n_scored].path, sizeof(scored[n_scored].path), "%s", path);
            scored[n_scored].score = s;
            /* FIX #2/#8: Cache the parsed cJSON entry to avoid re-reading top results */
            scored[n_scored].cached_entry = entry;
            n_scored++;
        } else {
            cJSON_Delete(entry);
        }
    }
    closedir(dir);

    /* Free query embedding */
    embed_vec_free(&query_emb);

    /* FIX #14: Use qsort instead of O(n²) bubble sort */
    qsort(scored, (size_t)n_scored, sizeof(scored_t), scored_cmp_desc);

    /* Load top results from cached entries (FIX #2/#8: no second file read) */
    int n = n_scored < max_results ? n_scored : max_results;
    results.entries = calloc((size_t)n, sizeof(memory_entry_t));
    results.count = 0;

    for (int i = 0; i < n; i++) {
        cJSON *entry = scored[i].cached_entry;
        if (!entry) continue;

        memory_entry_t *e = &results.entries[results.count];
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *v = cJSON_GetObjectItem(entry, "value");
        cJSON *tags = cJSON_GetObjectItem(entry, "tags");
        cJSON *p = cJSON_GetObjectItem(entry, "pinned");

        e->key = k && k->valuestring ? strdup(k->valuestring) : strdup("");
        e->value = v && v->valuestring ? strdup(v->valuestring) : strdup("");
        e->pinned = p ? cJSON_IsTrue(p) : 0;

        /* Copy tags */
        if (tags) {
            e->n_tags = cJSON_GetArraySize(tags);
            e->tags = calloc((size_t)e->n_tags, sizeof(char *));
            for (int t = 0; t < e->n_tags; t++) {
                cJSON *tag = cJSON_GetArrayItem(tags, t);
                e->tags[t] = tag && tag->valuestring ? strdup(tag->valuestring) : strdup("");
            }
        }

        /* Copy validation counters */
        cJSON *rh = cJSON_GetObjectItem(entry, "recall_hits");
        cJSON *rm = cJSON_GetObjectItem(entry, "recall_misses");
        e->recall_hits = rh ? (int)cJSON_GetNumberValue(rh) : 0;
        e->recall_misses = rm ? (int)cJSON_GetNumberValue(rm) : 0;

        /* Copy journal provenance reference */
        cJSON *jr = cJSON_GetObjectItem(entry, "journal_ref");
        e->journal_ref = (jr && jr->valuestring) ? strdup(jr->valuestring) : NULL;

        /* Update access_count and last_accessed */
        cJSON *ac = cJSON_GetObjectItem(entry, "access_count");
        if (ac) {
            cJSON_SetNumberValue(ac, cJSON_GetNumberValue(ac) + 1);
        }
        char ts[32];
        snprintf(ts, sizeof(ts), "%.5f", epoch_now());
        cJSON_ReplaceItemInObject(entry, "last_accessed", cJSON_CreateString(ts));

        /* Write back updated entry */
        char *json = cJSON_Print(entry);
        FILE *wf = fopen(scored[i].path, "w");
        if (wf) { fputs(json, wf); fclose(wf); }
        free(json);

        results.count++;
    }

    /* Free all cached entries (including non-top ones) */
    for (int i = 0; i < n_scored; i++) {
        cJSON_Delete(scored[i].cached_entry);
    }
    free(scored);
    return results;
}

/* ── build_index ─────────────────────────────────────── */

/* FIX #11: Sorted memory index — entries sorted alphabetically by key */
typedef struct {
    char *line;   /* formatted line: "  key [tags]\n" */
    char *key;    /* key for sorting */
} index_entry_t;

static int index_entry_cmp(const void *a, const void *b) {
    return strcmp(((const index_entry_t *)a)->key,
                 ((const index_entry_t *)b)->key);
}

char *memory_build_index(memory_t *m, int max_entries) {
    if (!m) return NULL;

    DIR *dir = opendir(m->dir);
    if (!dir) return NULL;

    /* Count entries by type for progressive disclosure */
    int n_lessons = 0, n_strategies = 0, n_facts = 0, n_tasks = 0, n_skills = 0, n_other = 0;
    int total = 0;

    /* Collect all entries for sorting (FIX #11) */
    int entries_cap = 64;
    index_entry_t *entries = calloc((size_t)entries_cap, sizeof(index_entry_t));

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *tags = cJSON_GetObjectItem(entry, "tags");

        if (k && k->valuestring) {
            /* Count by type */
            if (strncmp(k->valuestring, "lesson:", 7) == 0) n_lessons++;
            else if (strncmp(k->valuestring, "strategy:", 9) == 0) n_strategies++;
            else if (strncmp(k->valuestring, "fact:", 5) == 0) n_facts++;
            else if (strncmp(k->valuestring, "task:", 5) == 0) n_tasks++;
            else if (strncmp(k->valuestring, "skill:", 6) == 0) n_skills++;
            else n_other++;

            /* Build formatted line */
            str_t line = str_new(256);
            str_appendf(&line, "  %s", k->valuestring);
            if (tags && cJSON_GetArraySize(tags) > 0) {
                str_append_cstr(&line, " [");
                int n = cJSON_GetArraySize(tags);
                for (int i = 0; i < n; i++) {
                    cJSON *t = cJSON_GetArrayItem(tags, i);
                    if (i > 0) str_append_cstr(&line, ", ");
                    if (t && t->valuestring) str_append_cstr(&line, t->valuestring);
                }
                str_append_cstr(&line, "]");
            }
            str_append_cstr(&line, "\n");

            /* Grow entries array if needed */
            if (total >= entries_cap) {
                entries_cap *= 2;
                entries = realloc(entries, (size_t)entries_cap * sizeof(index_entry_t));
            }
            entries[total].line = str_steal(&line);
            entries[total].key = strdup(k->valuestring);
            total++;
        }
        cJSON_Delete(entry);
    }
    closedir(dir);

    if (total == 0) {
        free(entries);
        return NULL;
    }

    /* Sort entries alphabetically by key (FIX #11) */
    qsort(entries, (size_t)total, sizeof(index_entry_t), index_entry_cmp);

    /* Build output string */
    str_t out = str_new(2048);
    int show = (max_entries == 0) ? total : (total < max_entries ? total : max_entries);
    for (int i = 0; i < show; i++) {
        str_append_cstr(&out, entries[i].line);
    }

    /* Build header with topic summary */
    str_t result = str_new(2048);
    str_appendf(&result, "Memory: %d entries", total);
    if (n_lessons > 0) str_appendf(&result, ", %d lessons", n_lessons);
    if (n_strategies > 0) str_appendf(&result, ", %d strategies", n_strategies);
    if (n_skills > 0) str_appendf(&result, ", %d skills", n_skills);
    if (n_facts > 0) str_appendf(&result, ", %d facts", n_facts);
    if (n_tasks > 0) str_appendf(&result, ", %d tasks", n_tasks);
    if (n_other > 0) str_appendf(&result, ", %d other", n_other);
    str_append_cstr(&result, "\n");

    if (total > max_entries && max_entries > 0) {
        str_appendf(&result, "  (showing first %d of %d — use memory_recall to search)\n", max_entries, total);
    }
    str_append_cstr(&result, str_cstr(&out));
    str_free(&out);

    /* Free entries */
    for (int i = 0; i < total; i++) {
        free(entries[i].line);
        free(entries[i].key);
    }
    free(entries);

    return str_steal(&result);
}

/* — write MEMORY.md index file ——————————————————————— */

int memory_write_index_file(memory_t *m) {
    if (!m) return -1;

    char path[4096];
    snprintf(path, sizeof(path), "%s/../MEMORY.md", m->dir);

    char *index = memory_build_index(m, 0);  /* 0 = show all for MEMORY.md */

    FILE *f = fopen(path, "w");
    if (!f) { free(index); return -1; }

    fprintf(f, "# Nash Memory Index\n\n");
    fprintf(f, "Auto-generated — do not edit manually.\n");
    fprintf(f, "Use `memory_store` and `memory_recall` to manage.\n\n");
    if (index) {
        fprintf(f, "%s", index);
        free(index);
    } else {
        fprintf(f, "(empty)\n");
    }
    fclose(f);
    return 0;
}

/* ── load_pinned ─────────────────────────────────────── */

char *memory_load_pinned(memory_t *m) {
    if (!m) return NULL;

    DIR *dir = opendir(m->dir);
    if (!dir) return NULL;

    str_t out = str_new(1024);
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        cJSON *p = cJSON_GetObjectItem(entry, "pinned");
        if (p && cJSON_IsTrue(p)) {
            cJSON *k = cJSON_GetObjectItem(entry, "key");
            cJSON *v = cJSON_GetObjectItem(entry, "value");
            if (k && k->valuestring && v && v->valuestring) {
                if (count > 0) str_append_cstr(&out, "\n");
                str_appendf(&out, "[PINNED: %s]\n%s", k->valuestring, v->valuestring);
                count++;
            }
        }
        cJSON_Delete(entry);
    }
    closedir(dir);

    if (count == 0) {
        str_free(&out);
        return NULL;
    }
    return str_steal(&out);
}

/* ── delete ──────────────────────────────────────────── */

int memory_delete(memory_t *m, const char *key) {
    if (!m || !key) return -1;

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    /* Check if entry exists */
    struct stat st;
    if (stat(path, &st) != 0) return -1;  /* not found */

    /* Remove JSON file */
    unlink(path);

    /* Remove embedding file if it exists */
    char emb_fname[512];
    key_to_emb_filename(key, emb_fname, sizeof(emb_fname));
    char emb_path[4096];
    snprintf(emb_path, sizeof(emb_path), "%s/%s", m->dir, emb_fname);
    unlink(emb_path);  /* ignore error if not exists */

    /* Update MEMORY.md index */
    memory_write_index_file(m);

    /* Git commit */
    char msg[256];
    snprintf(msg, sizeof(msg), "memory: delete %s", key);
    memory_git_commit(m, msg);

    return 0;
}

/* ── free results ────────────────────────────────────── */

void memory_results_free(memory_results_t *r) {
    if (!r || !r->entries) return;
    for (int i = 0; i < r->count; i++) {
        free(r->entries[i].key);
        free(r->entries[i].value);
        free(r->entries[i].journal_ref);
        for (int t = 0; t < r->entries[i].n_tags; t++)
            free(r->entries[i].tags[t]);
        free(r->entries[i].tags);
    }
    free(r->entries);
    r->entries = NULL;
    r->count = 0;
}

/* ── prune (forgetting/decay) ─────────────────────────────── */

int memory_prune(memory_t *m, double min_score, int min_evidence) {
    if (!m) return 0;

    DIR *dir = opendir(m->dir);
    if (!dir) return 0;

    int pruned = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        /* Never prune pinned memories */
        cJSON *pin = cJSON_GetObjectItem(entry, "pinned");
        if (pin && cJSON_IsTrue(pin)) { cJSON_Delete(entry); continue; }

        /* Bayesian validation scoring — prune only with sufficient evidence.
         * Age is NOT a criterion: a year-old lesson with no evidence is
         * unknown (score 0.50), not worthless. Only prune when the data
         * shows the memory is actively harmful (recalled in failed tasks).
         * score = (hits+1)/(hits+misses+2) — Beta posterior mean. */
        cJSON *rh = cJSON_GetObjectItem(entry, "recall_hits");
        cJSON *rm = cJSON_GetObjectItem(entry, "recall_misses");
        int hits = rh ? (int)cJSON_GetNumberValue(rh) : 0;
        int misses = rm ? (int)cJSON_GetNumberValue(rm) : 0;
        int evidence = hits + misses;
        double vscore = (hits + 1.0) / (hits + misses + 2.0);

        if (vscore < min_score && evidence >= min_evidence) {
            unlink(path);
            pruned++;
        }

        cJSON_Delete(entry);
    }
    closedir(dir);

    /* Git: commit pruning results */
    if (pruned > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "memory: prune %d entries (score < threshold)", pruned);
        memory_git_commit(m, msg);
    }

    return pruned;
}

/* ── validation scoring ─────────────────────────────────────── */

/* Internal: increment a numeric field in a memory entry's JSON file */
static int memory_increment_field(memory_t *m, const char *key,
                                   const char *field) {
    if (!m || !key || !field) return -1;

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* entry not found */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *entry = cJSON_Parse(buf);
    free(buf);
    if (!entry) return -1;

    /* Increment the field (create if missing) */
    cJSON *fld = cJSON_GetObjectItem(entry, field);
    if (fld) {
        cJSON_SetNumberValue(fld, cJSON_GetNumberValue(fld) + 1);
    } else {
        cJSON_AddNumberToObject(entry, field, 1);
    }

    /* Write back */
    char *json = cJSON_Print(entry);
    f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);

    /* Git: commit validation score update */
    char cmsg[256];
    snprintf(cmsg, sizeof(cmsg), "memory: update %s for %s", field, key);
    memory_git_commit(m, cmsg);

    return 0;
}

int memory_increment_hits(memory_t *m, const char *key) {
    return memory_increment_field(m, key, "recall_hits");
}

int memory_increment_misses(memory_t *m, const char *key) {
    return memory_increment_field(m, key, "recall_misses");
}

/* ── embedding integration ──────────────────────────────────── */

int memory_init_embeddings(memory_t *m, const char *type,
                           const char *model, const char *api_base,
                           const char *model_path, int dimension) {
    if (!m || !type) return 0;

    /* Expand ~ in model_path */
    char expanded_path[4096];
    const char *resolved_model_path = model_path;
    if (model_path && model_path[0] == '~' && (model_path[1] == '/' || model_path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, model_path + 1);
            resolved_model_path = expanded_path;
        }
    }

    /* Parse embedding type */
    embed_config_t cfg = {0};
    if (strcmp(type, "onnx") == 0) {
        cfg.type = EMBED_ONNX;
        cfg.model_path = (char *)(resolved_model_path ? resolved_model_path : NULL);
    } else if (strcmp(type, "ollama") == 0) {
        cfg.type = EMBED_OLLAMA;
        cfg.api_base = (char *)(api_base ? api_base : "http://localhost:11434");
        cfg.model = (char *)(model ? model : "nomic-embed-text");
    } else if (strcmp(type, "openai") == 0) {
        cfg.type = EMBED_OPENAI;
        cfg.api_base = (char *)(api_base ? api_base : "http://localhost:8080");
        cfg.model = (char *)(model ? model : "text-embedding-3-small");
    } else {
        /* "none" or unknown — embeddings disabled */
        return 0;
    }
    cfg.dimension = dimension;

    /* Create embedding context */
    m->embed = embed_new(&cfg);
    if (!m->embed) return 0;

    /* Probe the backend — if it's not available, gracefully disable */
    if (!embed_probe(m->embed)) {
        if (cfg.type == EMBED_ONNX) {
            fprintf(stderr, "[memory] ONNX embedding at %s not available, "
                    "falling back to substring matching\n",
                    model_path ? model_path : "(no path)");
        } else {
            fprintf(stderr, "[memory] embedding service at %s not available, "
                    "falling back to substring matching\n", cfg.api_base);
        }
        embed_free(m->embed);
        m->embed = NULL;
        return 0;
    }

    if (cfg.type == EMBED_ONNX) {
        fprintf(stderr, "[memory] ONNX embeddings enabled: %s (dim=%d)\n",
                model_path, m->embed->detected_dim);
    } else {
        fprintf(stderr, "[memory] semantic embeddings enabled: %s/%s (dim=%d)\n",
                cfg.api_base, cfg.model, m->embed->detected_dim);
    }

    /* On first enable, embed any existing memories that lack .emb files */
    int embedded = memory_embed_all(m);
    if (embedded > 0) {
        fprintf(stderr, "[memory] generated embeddings for %d existing memories\n",
                embedded);
    }

    return 1;
}

int memory_embed_entry(memory_t *m, const char *key, const char *value,
                       const char **tags, int n_tags) {
    if (!m || !m->embed || !m->embed->available || !key) return -1;

    /* Prepare text for embedding */
    char *text = embed_prepare_text(key, value, tags, n_tags, 2000);
    if (!text) return -1;

    /* Generate embedding */
    embed_vec_t vec = embed_text(m->embed, text);
    free(text);

    if (!vec.data || vec.dim <= 0) {
        embed_vec_free(&vec);
        return -1;
    }

    /* Save to .emb file */
    char emb_fname[512];
    key_to_emb_filename(key, emb_fname, sizeof(emb_fname));

    char emb_path[4096];
    snprintf(emb_path, sizeof(emb_path), "%s/%s", m->dir, emb_fname);

    int rc = embed_vec_save(&vec, emb_path);
    embed_vec_free(&vec);
    return rc;
}

int memory_embed_all(memory_t *m) {
    if (!m || !m->embed || !m->embed->available) return 0;

    DIR *dir = opendir(m->dir);
    if (!dir) return 0;

    int embedded = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        /* Check if .emb file already exists */
        char json_path[4096], emb_path[4096];
        snprintf(json_path, sizeof(json_path), "%s/%s", m->dir, de->d_name);
        json_to_emb_path(json_path, emb_path, sizeof(emb_path));

        struct stat st;
        if (stat(emb_path, &st) == 0) continue;  /* already has embedding */

        /* Load JSON entry */
        FILE *f = fopen(json_path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *v = cJSON_GetObjectItem(entry, "value");
        cJSON *t = cJSON_GetObjectItem(entry, "tags");

        const char *ekey = (k && k->valuestring) ? k->valuestring : "";
        const char *eval = (v && v->valuestring) ? v->valuestring : "";

        /* Extract tags as string array */
        int nt = t ? cJSON_GetArraySize(t) : 0;
        const char **tag_strs = NULL;
        if (nt > 0) {
            tag_strs = malloc(sizeof(char *) * (size_t)nt);
            for (int i = 0; i < nt; i++) {
                cJSON *ti = cJSON_GetArrayItem(t, i);
                tag_strs[i] = (ti && ti->valuestring) ? ti->valuestring : "";
            }
        }

        if (memory_embed_entry(m, ekey, eval, tag_strs, nt) == 0)
            embedded++;

        free(tag_strs);
        cJSON_Delete(entry);
    }
    closedir(dir);

    return embedded;
}
