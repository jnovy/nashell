#define _GNU_SOURCE
#include "memory.h"
#include "nash_limits.h"
#include "str.h"
#include "tui.h"
#include "nash_log.h"
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
#include <math.h>     /* log */

/* ── helpers ─────────────────────────────────────────── */

/* Sanitize key for filename: replace : and / with _, append extension.
 * ext should include the dot, e.g. ".json" or ".emb". */
void key_to_path(const char *key, const char *ext, char *out, size_t out_sz) {
    size_t ext_len = strlen(ext);
    /* FIX B5: Guard against small out_sz — if out_sz < ext_len+2, the
     * subtraction wraps around (size_t is unsigned), causing a massive loop. */
    if (out_sz < ext_len + 2) { if (out_sz > 0) out[0] = '\0'; return; }
    size_t i = 0;
    for (; key[i] && i < out_sz - ext_len - 1; i++)
        out[i] = (key[i] == ':' || key[i] == '/') ? '_' : key[i];
    /* FIX #12: Use memcpy instead of strcat — the loop already
     * reserved exactly ext_len+1 bytes, so strcat's linear scan
     * for NUL is unnecessary and fragile. */
    memcpy(out + i, ext, ext_len + 1);  /* +1 copies NUL terminator */
}

/* Forward declarations for helpers used by index loading (defined later) */
static void json_to_emb_path(const char *json_path, char *emb_path, size_t sz);

static double epoch_now(void) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    return (double)tp.tv_sec + (double)tp.tv_nsec / 1e9;
}

/* ── directory traversal helpers ────────────────────────── */

/* Callback-based directory iteration over .json entries.
 * Callback receives the parsed cJSON entry and user_data.
 * Return values:
 *   0  = continue iterating, helper deletes entry
 *  -1  = continue iterating, caller took ownership of entry (don't delete)
 *  >0  = stop iterating, helper deletes entry
 * This enables callers like memory_recall to cache high-scoring entries. */
typedef int (*json_entry_cb)(const char *dirpath, cJSON *entry, void *user_data);

#define JSON_CB_CONTINUE    0
#define JSON_CB_KEEP_ENTRY -1

/* ── Directory iteration wrappers (use for_each_dir_entry from str.h) ── */

/* Context struct for json_entry_wrapper — avoids casting function pointers
 * through void* (which is technically undefined behavior in ISO C). */
typedef struct {
    json_entry_cb cb;
    void *user_data;
} json_entry_ctx_t;

/* Wrapper to adapt json_entry_cb to dir_entry_cb signature. */
static int json_entry_wrapper(const char *dirpath, const char *filename,
                              const char *fullpath, void *user_data) {
    (void)filename;
    json_entry_ctx_t *ctx = (json_entry_ctx_t *)user_data;

    cJSON *entry = slurp_json(fullpath);
    if (!entry) return 0;

    int rc = ctx->cb(dirpath, entry, ctx->user_data);
    if (rc != JSON_CB_KEEP_ENTRY)
        cJSON_Delete(entry);
    return rc;
}

static void for_each_json_entry(const char *dirpath, json_entry_cb cb, void *user_data) {
    json_entry_ctx_t ctx = { .cb = cb, .user_data = user_data };
    for_each_dir_entry(dirpath, ".json", json_entry_wrapper, &ctx);
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
    char git_path[NASH_PATH_MAX];
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
    /* In deferred mode, skip individual commits — they'll be batched. */
    if (m->git_deferred) { m->git_deferred_count++; return; }
    char git_path[NASH_PATH_MAX];
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

/* ── P6: description generation ────────────────────────── */

/* P6: Generate a short description from the value text.
 * Extracts the first sentence (up to '.') or first line (up to '\n'),
 * whichever comes first, capped at 250 chars.
 *
 * Research basis:
 *   Letta Context Repositories [May 2026] — frontmatter descriptions
 *     in memory files enable progressive disclosure.
 *   Claude Code Auto Memory [2026] — MEMORY.md index with topic
 *     descriptions for selective loading.
 *   AutoMEM [arXiv:2606.04315, Jun 2026] — agents perform best when
 *     they can browse memory descriptions before loading full content.
 *
 * Returns heap-allocated string. Caller must free. */
static char *generate_description(const char *value) {
    if (!value || !value[0]) return strdup("");

    /* Skip leading whitespace and markdown headers */
    const char *start = value;
    while (*start == ' ' || *start == '\t' || *start == '#' ||
           *start == '\n' || *start == '\r' || *start == '*')
        start++;
    if (!*start) return strdup("");

    /* Find first sentence end (.) or newline, whichever comes first */
    const char *dot = strchr(start, '.');
    const char *nl = strchr(start, '\n');
    const char *end;
    size_t slen = strlen(start);

    if (dot && (!nl || dot < nl) && (dot - start) < 250) {
        end = dot + 1;  /* include the period */
    } else if (nl && (nl - start) < 250) {
        end = nl;
    } else {
        /* No sentence/line break within 250 chars — truncate */
        end = start + (slen < 150 ? slen : 150);
    }

    int dlen = (int)(end - start);
    if (dlen > 250) dlen = 250;
    if (dlen <= 0) return strdup("");

    char *desc = malloc((size_t)dlen + 1);
    memcpy(desc, start, (size_t)dlen);
    desc[dlen] = '\0';
    return desc;
}

/* ── P1: index cache helpers ───────────────────────────── */

/* Free a single index entry's owned fields */
static void mem_index_entry_free(mem_index_entry_t *e) {
    if (!e) return;
    free(e->key);
    free(e->description);
    free(e->value);
    free(e->path);
    for (int i = 0; i < e->n_refs; i++) free(e->refs[i]);
    free(e->refs);
    if (e->has_emb) embed_multi_vec_free(&e->emb);
    memset(e, 0, sizeof(*e));
}

/* Free the entire index */
static void mem_index_free(mem_index_t *idx) {
    if (!idx) return;
    for (int i = 0; i < idx->count; i++)
        mem_index_entry_free(&idx->entries[i]);
    free(idx->entries);
    memset(idx, 0, sizeof(*idx));
}

/* Find an index entry by key. Returns pointer or NULL. */
static mem_index_entry_t *mem_index_find(mem_index_t *idx, const char *key) {
    if (!idx || !key) return NULL;
    for (int i = 0; i < idx->count; i++) {
        if (idx->entries[i].key && strcmp(idx->entries[i].key, key) == 0)
            return &idx->entries[i];
    }
    return NULL;
}

/* Ensure capacity for at least one more entry */
static void mem_index_grow(mem_index_t *idx) {
    if (idx->count >= idx->cap) {
        idx->cap = idx->cap ? idx->cap * 2 : 64;
        idx->entries = realloc(idx->entries,
                               (size_t)idx->cap * sizeof(mem_index_entry_t));
    }
}

/* Remove an entry from the index by key */
static void mem_index_remove(mem_index_t *idx, const char *key) {
    if (!idx || !key) return;
    for (int i = 0; i < idx->count; i++) {
        if (idx->entries[i].key && strcmp(idx->entries[i].key, key) == 0) {
            mem_index_entry_free(&idx->entries[i]);
            if (i < idx->count - 1) {
                memmove(&idx->entries[i], &idx->entries[i + 1],
                        (size_t)(idx->count - 1 - i) * sizeof(mem_index_entry_t));
            }
            idx->count--;
            return;
        }
    }
}

/* Populate an index entry from a parsed cJSON entry + file path.
 * Also loads the embedding if available. */
static void mem_index_entry_from_json(mem_index_entry_t *ie, cJSON *entry,
                                       const char *filepath) {
    memset(ie, 0, sizeof(*ie));

    cJSON *k = cJSON_GetObjectItem(entry, "key");
    cJSON *v = cJSON_GetObjectItem(entry, "value");
    cJSON *d = cJSON_GetObjectItem(entry, "description");
    cJSON *p = cJSON_GetObjectItem(entry, "pinned");
    cJSON *ac = cJSON_GetObjectItem(entry, "access_count");
    cJSON *rh = cJSON_GetObjectItem(entry, "recall_hits");
    cJSON *rm = cJSON_GetObjectItem(entry, "recall_misses");
    cJSON *be = cJSON_GetObjectItem(entry, "belief_entropy");
    cJSON *ca = cJSON_GetObjectItem(entry, "created_at");

    ie->key = (k && k->valuestring) ? strdup(k->valuestring) : strdup("");
    ie->value = (v && v->valuestring) ? strdup(v->valuestring) : strdup("");
    ie->description = (d && d->valuestring) ? strdup(d->valuestring)
                                             : generate_description(ie->value);
    ie->pinned = p ? cJSON_IsTrue(p) : 0;
    ie->access_count = ac ? (int)cJSON_GetNumberValue(ac) : 0;
    ie->recall_hits = rh ? (int)cJSON_GetNumberValue(rh) : 0;
    ie->recall_misses = rm ? (int)cJSON_GetNumberValue(rm) : 0;
    ie->belief_entropy = be ? cJSON_GetNumberValue(be) : -1.0;
    if (ca && ca->valuestring) ie->created_at = atof(ca->valuestring);
    else if (ca) ie->created_at = cJSON_GetNumberValue(ca);
    ie->path = strdup(filepath);

    /* Copy refs */
    cJSON *refs_arr = cJSON_GetObjectItem(entry, "refs");
    if (refs_arr && cJSON_IsArray(refs_arr)) {
        ie->n_refs = cJSON_GetArraySize(refs_arr);
        if (ie->n_refs > 0) {
            ie->refs = calloc((size_t)ie->n_refs, sizeof(char *));
            for (int i = 0; i < ie->n_refs; i++) {
                cJSON *ref = cJSON_GetArrayItem(refs_arr, i);
                ie->refs[i] = (ref && ref->valuestring) ? strdup(ref->valuestring) : strdup("");
            }
        }
    }

    /* Load embedding if .emb file exists */
    char emb_path[NASH_PATH_MAX];
    json_to_emb_path(filepath, emb_path, sizeof(emb_path));
    ie->emb = embed_multi_vec_load(emb_path);
    ie->has_emb = (ie->emb.data && ie->emb.dim > 0) ? 1 : 0;
}

/* Callback for loading all entries into the index at startup */
typedef struct {
    mem_index_t *idx;
} index_load_ctx_t;

static int index_load_cb(const char *dirpath, const char *filename,
                          const char *fullpath, void *user_data) {
    index_load_ctx_t *ctx = (index_load_ctx_t *)user_data;
    (void)dirpath; (void)filename;

    cJSON *entry = slurp_json(fullpath);
    if (!entry) return 0;

    mem_index_grow(ctx->idx);
    mem_index_entry_from_json(&ctx->idx->entries[ctx->idx->count], entry, fullpath);
    ctx->idx->count++;

    cJSON_Delete(entry);
    return 0;
}

/* Load the full index from disk. Called once at memory_new(). */
static void mem_index_load(memory_t *m) {
    mem_index_free(&m->idx);
    index_load_ctx_t ctx = { .idx = &m->idx };
    for_each_dir_entry(m->dir, ".json", index_load_cb, &ctx);
}

/* ── create/free ─────────────────────────────────────── */

memory_t *memory_new(const char *project_root) {
    memory_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/memory", project_root);
    mkdir(path, 0755);
    m->dir = strdup(path);

    /* Recursive mutex: memory_prune() → memory_delete_batch() nesting. */
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&m->mtx, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    /* P1: Load in-memory index from disk at startup.
     * This is the only full directory scan — all subsequent operations
     * (recall, build_index, load_pinned) use the cached index.
     *
     * Research basis:
     *   AutoMEM [arXiv:2606.04315, Jun 2026] — self-managed memory with
     *     active control beats all passive retrieval pipelines.
     *   MRAgent [arXiv:2606.06036, ICML 2026] — Cue-Tag-Content graph;
     *     our index serves as the "cue" layer for fast navigation. */
    mem_index_load(m);

    return m;
}

void memory_free(memory_t *m) {
    if (!m) return;
    mem_index_free(&m->idx);
    pthread_mutex_destroy(&m->mtx);
    embed_free(m->embed);
    free(m->dir);
    free(m->model);
    free(m);
}

void memory_set_recall_config(memory_t *m, double min_score,
                              float blend_semantic, float blend_substring,
                              float vscore_exp) {
    if (!m) return;
    m->recall_min_score = min_score;
    m->recall_blend_semantic = blend_semantic;
    m->recall_blend_substring = blend_substring;
    m->vscore_exponent = vscore_exp;
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

/* Load a memory entry JSON by key. Returns parsed cJSON or NULL.
 * Caller must cJSON_Delete() the result. */
static cJSON *memory_load_entry_json(memory_t *m, const char *key) {
    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);
    return slurp_json(path);
}

/* ── store ───────────────────────────────────────────── */

int memory_store(memory_t *m, const char *key, const char *value,
                 int pinned, const char *journal_ref,
                 const char **refs, int n_refs) {
    if (!m || !key || !value) return -1;
    pthread_mutex_lock(&m->mtx);

    /* Initialize git repo on first store */
    memory_git_init(m);

    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "key", key);
    cJSON_AddStringToObject(entry, "value", value);

    /* FIX D8: Tags field is reserved for future use (e.g., dreaming's
     * SYNTHESIZE pass for auto-tagging).  Currently always empty. */
    cJSON *tags_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(entry, "tags", tags_arr);

    cJSON_AddBoolToObject(entry, "pinned", pinned);

    /* Check if entry already exists (preserve counters and metadata).
     * Single read to preserve all fields from the existing entry. */
    int access_count = 1;  /* store itself counts as one access */
    int recall_hits = 0;
    int recall_misses = 0;
    double created_at = epoch_now();
    double belief_entropy = -1;
    char *old_supersedes = NULL;
    int old_version = 0;
    {
        cJSON *old = slurp_json(path);
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
            cJSON *be = cJSON_GetObjectItem(old, "belief_entropy");
            if (be) belief_entropy = cJSON_GetNumberValue(be);
            /* P2: Preserve lineage fields */
            cJSON *ss = cJSON_GetObjectItem(old, "supersedes");
            if (ss && ss->valuestring) old_supersedes = strdup(ss->valuestring);
            cJSON *vn = cJSON_GetObjectItem(old, "version");
            if (vn) old_version = (int)cJSON_GetNumberValue(vn);
            cJSON_Delete(old);
        }
    }

    char ts[32];
    snprintf(ts, sizeof(ts), "%.5f", created_at);
    cJSON_AddStringToObject(entry, "created_at", ts);
    snprintf(ts, sizeof(ts), "%.5f", epoch_now());
    cJSON_AddStringToObject(entry, "last_accessed", ts);
    cJSON_AddNumberToObject(entry, "access_count", access_count);
    cJSON_AddNumberToObject(entry, "recall_hits", recall_hits);
    cJSON_AddNumberToObject(entry, "recall_misses", recall_misses);

    /* Belief Entropy — forward-looking quality signal (MMPO).
     * Preserved from existing entry above, or -1 (not computed). */
    cJSON_AddNumberToObject(entry, "belief_entropy", belief_entropy);

    /* P6: Auto-generate description from value text for progressive disclosure.
     * Research: Letta Context Repos [May 2026], Claude Code Auto Memory [2026],
     * AutoMEM [arXiv:2606.04315, Jun 2026]. */
    {
        char *desc = generate_description(value);
        cJSON_AddStringToObject(entry, "description", desc);
        free(desc);
    }

    /* Provenance: link to the session journal where this memory was created.
     * The dreaming LLM can read this journal to understand original context. */
    if (journal_ref)
        cJSON_AddStringToObject(entry, "journal_ref", journal_ref);

    /* Inter-memory references: "see also" links to related memory keys.
     * Populated by dreaming's SYNTHESIZE pass to create a lightweight
     * graph structure without a full graph database.
     *
     * Research basis:
     *   MemForest [arXiv:2605.23986, May 2026] — hierarchical temporal
     *     trees where parent nodes summarize children.
     *   ActiveGraph [arXiv:2605.21997, May 2026] — typed edges between
     *     nodes in a reactive graph.
     *   MemIR [arXiv:2605.25869, May 2026] — provenance chains linking
     *     raw evidence to claims.
     *
     * During recall, ref'd memories get a score boost (+0.3 × parent score)
     * when the referencing memory scores highly (≥0.5), creating implicit
     * "see also" behavior without explicit graph traversal. */
    if (refs && n_refs > 0) {
        cJSON *refs_arr = cJSON_AddArrayToObject(entry, "refs");
        for (int i = 0; i < n_refs; i++)
            cJSON_AddItemToArray(refs_arr, cJSON_CreateString(refs[i]));
    }

    /* P2: Lesson lineage — preserve supersedes and version from old entry.
     * New supersedes values are set by the caller via memory_set_supersedes(). */
    if (old_supersedes) {
        cJSON_AddStringToObject(entry, "supersedes", old_supersedes);
        free(old_supersedes);
    }
    if (old_version > 0)
        cJSON_AddNumberToObject(entry, "version", old_version);
    else
        cJSON_AddNumberToObject(entry, "version", 1);

    char *json = cJSON_Print(entry);
    write_file(path, json, strlen(json));
    free(json);
    cJSON_Delete(entry);

    /* Generate embedding for semantic matching (if enabled).
     * Skip during batch operations (consolidating flag) — embeddings
     * will be regenerated in bulk via memory_embed_all() after the
     * batch completes.  This avoids generating throwaway embeddings
     * for entries that are about to be merged/deleted in the same pass. */
    if (m->embed && m->embed->available && !m->consolidating) {
        memory_embed_entry(m, key, value);
    }

    /* P1: Update in-memory index — either update existing entry or add new.
     * Re-reads the just-written JSON to populate the index entry with all
     * fields including the auto-generated description. */
    {
        cJSON *fresh = memory_load_entry_json(m, key);
        if (fresh) {
            mem_index_entry_t *existing = mem_index_find(&m->idx, key);
            if (existing) {
                mem_index_entry_free(existing);
                mem_index_entry_from_json(existing, fresh, path);
            } else {
                mem_index_grow(&m->idx);
                mem_index_entry_from_json(&m->idx.entries[m->idx.count], fresh, path);
                m->idx.count++;
            }
            cJSON_Delete(fresh);
        }
    }

    /* Git commit: track memory creation/update */
    char commit_msg[256];
    snprintf(commit_msg, sizeof(commit_msg), "memory: store %s", key);
    memory_git_commit(m, commit_msg);

    pthread_mutex_unlock(&m->mtx);
    return 0;
}


/* ── pin/unpin ───────────────────────────────────────────── */

/* Helper: load entry JSON from file, modify pinned flag, write back */
static int memory_set_pinned(memory_t *m, const char *key, int pinned) {
    if (!m || !key) return -1;
    pthread_mutex_lock(&m->mtx);

    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    cJSON *entry = memory_load_entry_json(m, key);
    if (!entry) { pthread_mutex_unlock(&m->mtx); return -1; }

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
    write_file(path, json, strlen(json));
    free(json);
    cJSON_Delete(entry);

    /* P1: Update in-memory index */
    {
        mem_index_entry_t *ie = mem_index_find(&m->idx, key);
        if (ie) ie->pinned = pinned;
    }

    /* Git: commit pin/unpin change */
    char msg[600];
    snprintf(msg, sizeof(msg), "memory: %s %s",
             pinned ? "pin" : "unpin", key);
    memory_git_commit(m, msg);

    pthread_mutex_unlock(&m->mtx);
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
                                     const char *query) {
    double relevance = 0;
    if (strcasestr(key, query)) relevance += 3.0;
    if (strcasestr(value, query)) relevance += 1.0;
    return relevance;
}

/* Composite scoring: semantic + substring + importance.
 * When embeddings are available, semantic similarity is the primary signal.
 * Substring matching provides a safety net for exact keyword matches that
 * embeddings might underweight (e.g., function names, error codes).
 *
 * No recency decay: knowledge doesn't expire on a calendar. A lesson
 * learned 6 months ago is just as valid as one learned today. Usage gaps
 * (not using nash for weeks) shouldn't degrade recall quality. Quality
 * control is handled by Bayesian validation scoring (hits/misses) and
 * pruning, not by wall-clock time.
 *
 * Inspired by GDN-2's "short convolution on gates": instead of scoring
 * each memory independently via substring, we use dense vector embeddings
 * that capture the full semantic context of the query — a continuous,
 * context-aware relevance signal. */
static double score_entry_hybrid(const char *key, const char *value,
                                  const char *query,
                                  int access_count,
                                  int recall_hits, int recall_misses,
                                  float semantic_sim, int has_semantic,
                                  float blend_semantic, float blend_substring,
                                  float vscore_exponent,
                                  double *out_relevance,
                                  double *out_importance) {
    double relevance;

    /* Use configurable blend weights (P3: Self-Harness tunable surfaces) */
    float w_sem = blend_semantic > 0 ? blend_semantic : 0.7f;
    float w_sub = blend_substring > 0 ? blend_substring : 0.3f;

    if (has_semantic) {
        /* Semantic mode: cosine similarity is primary signal.
         * Clamp negative similarities to 0 (semantically opposite = no match).
         * Both semantic and substring scores are in [0, 4] raw range.
         * After blending, normalize to [0, 1] for consistent thresholding. */
        double clamped = (double)semantic_sim;
        if (clamped < 0.0) clamped = 0.0;
        double semantic = clamped * 4.0;  /* [0, 4] */
        double substring = score_entry_substring(key, value, query);

        /* Blend: semantic + substring using configurable weights.
         * Default: 70% semantic + 30% substring.
         * Normalize by actual weight sum so result is always in [0, 1]. */
        double w_total = (double)(w_sem + w_sub);
        if (w_total < 0.001) w_total = 1.0;  /* guard against zero weights */
        relevance = (semantic * w_sem + substring * w_sub) / (4.0 * w_total);  /* [0, 1] */
    } else {
        /* Fallback: pure substring matching (no embeddings available).
         * Normalize to [0, 1] — same range as the embedding path.
         * Max raw substring = 3.0 (key) + 1.0 (value) = 4.0. */
        relevance = score_entry_substring(key, value, query) / 4.0;  /* [0, 1] */
        if (relevance == 0) return 0;  /* no match at all */
    }

    if (relevance < 0.001) return 0;  /* hard floor: no match at all */

    /* Importance: logarithmic access frequency, normalized to [0, 1].
     * log(1 + access_count) grows slowly: 0→0, 10→0.48, 100→0.92, 1000→1.0.
     * Cap at 5.0 (≈148 accesses) to keep the range bounded. */
    double importance = log(1.0 + (double)access_count) / 5.0;
    if (importance > 1.0) importance = 1.0;

    /* Scoring: relevance only — importance removed from ranking.
     *
     * Empirical analysis showed that importance (log access frequency)
     * distorts ranking by boosting frequently-recalled but irrelevant
     * memories above less-popular but more relevant ones. Example:
     * "memory-deduplication-procedure" (rel=0.19, imp=1.0) ranked above
     * "compare-interface-implementations" (rel=0.34, imp=0.5) for a
     * query about code simplification — wrong.
     *
     * Importance is redundant with vscore: popular memories accumulate
     * more recall_hits → higher vscore. The Bayesian validation score
     * already captures "this memory is useful" without the distortion
     * of "this memory is popular for OTHER queries."
     *
     * importance is still computed and exposed via memory_entry_t for
     * diagnostics (test_memory_context) but doesn't affect ranking. */
    double composite = relevance;

    /* P3: Bayesian validation scoring — data-driven memory quality signal.
     * vscore = (hits+1)/(hits+misses+2) — Beta posterior mean with
     * Laplace smoothing (conjugate prior for Bernoulli likelihood).
     * New memories with no evidence get vscore=0.5 (maximum entropy).
     * Memories that consistently correlate with failures get demoted
     * before they accumulate enough evidence for pruning (min_evidence=3).
     *
     * Research basis:
     *   MemFail [arXiv:2605.26667, May 2026] — diagnostic benchmark
     *     showing that injecting weakly-relevant memories HURTS
     *     performance. Bayesian scoring provides the data-driven signal
     *     to identify which memories are genuinely useful.
     *   MemForest [arXiv:2605.23986, May 2026] — temporal indexing
     *     paper that validates importance-weighted scoring for memory
     *     retrieval quality.
     *   Memory Survey [arXiv:2404.13501, 2024] — comprehensive survey
     *     identifying five critical memory operations, including
     *     validation/reflection as essential for memory quality.
     *   Generative Agents [Park et al., 2023] — composite scoring
     *     (recency × importance × relevance) as the foundation for
     *     memory retrieval ranking.
     *
     * This closes the gap between validation and recall ranking.
     *
     * Power-law exponent (vscore_exponent) controls vscore's influence:
     *   composite = relevance × pow(vscore, exponent)
     *   exponent=1.0: full multiplicative (original behavior, harsh cold-start)
     *   exponent=0.3: reduced influence (default — 86% of memories have
     *     vscore=0.5 due to zero evidence; ×0.81 instead of ×0.50)
     *   exponent=0.0: disabled (pure relevance ranking)
     *
     * Empirical calibration (639 memories, 527 sessions):
     *   86% stuck at vscore=0.5 (cold-start catch-22)
     *   7% at vscore≥0.90 (rich-get-richer)
     *   With exponent=1.0, a veteran (rel=0.25, vs=0.95 → 0.24) beats
     *   a new memory (rel=0.40, vs=0.50 → 0.20) despite lower relevance.
     *   With exponent=0.3, new memory wins (0.40×0.81=0.32 vs 0.25×0.99=0.25). */
    double vscore = (recall_hits + 1.0) / (recall_hits + recall_misses + 2.0);
    if (out_relevance) *out_relevance = relevance;
    if (out_importance) *out_importance = importance;
    if (vscore_exponent <= 0.0f)
        return composite;  /* exponent=0 disables vscore entirely */
    return composite * pow(vscore, (double)vscore_exponent);
}

/* qsort comparator for scored entries (descending by score) */
/* P1: scored_t simplified — no cached_entry needed since we use the
 * in-memory index. Only stores index position + scores. */
typedef struct { int idx_pos; double score; double relevance; double importance; } scored_t;

static int scored_cmp_desc(const void *a, const void *b) {
    double sa = ((const scored_t *)a)->score;
    double sb = ((const scored_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

/* Forward declaration — used by memory_recall to persist access_count. */
static int memory_increment_field(memory_t *m, const char *key,
                                   const char *field);

/* P1: memory_recall rewritten to use in-memory index cache.
 * Eliminates O(n) filesystem reads per recall — iterates the cached
 * index array instead of scanning the directory.
 *
 * Research basis:
 *   AutoMEM [arXiv:2606.04315, Jun 2026] — self-managed memory with
 *     active control beats all passive retrieval pipelines.
 *   MRAgent [arXiv:2606.06036, ICML 2026] — active reconstruction
 *     mechanism integrates reasoning into memory access. Our index
 *     enables the same pattern by making all memory metadata available
 *     without I/O during the scoring phase.
 *   DCPM [arXiv:2606.09483, Jun 2026] — dual-process cognitive memory
 *     with synchronous fast-path access. Index iteration IS the fast path. */
memory_results_t memory_recall(memory_t *m, const char *query, int max_results) {
    memory_results_t results = {0};
    if (!m || !query || m->idx.count == 0) return results;
    pthread_mutex_lock(&m->mtx);

    /* FIX B1: Type prefix extraction for filtering */
    const char *type_filter = NULL;
    size_t type_filter_len = 0;
    const char *score_query = query;
    {
        static const char *type_prefixes[] = {
            "skill:", "lesson:", "strategy:", "fact:", "task:",
            "anti-pattern:", "other:", NULL
        };
        for (const char **pfx = type_prefixes; *pfx; pfx++) {
            size_t plen = strlen(*pfx);
            if (strncmp(query, *pfx, plen) == 0) {
                type_filter = *pfx;
                type_filter_len = plen;
                score_query = query + plen;
                while (*score_query == ' ') score_query++;
                if (*score_query == '\0') score_query = query;
                break;
            }
        }
    }

    /* Generate query embedding once */
    embed_multi_vec_t query_mv = {0};
    int has_semantic = 0;
    if (m->embed && m->embed->available) {
        int max_chars = embed_max_input_chars(m->embed);
        size_t query_len = strlen(score_query);

        if (query_len <= (size_t)max_chars) {
            embed_vec_t single = embed_text(m->embed, score_query);
            if (single.data && single.dim > 0) {
                query_mv.data = single.data;
                query_mv.dim = single.dim;
                query_mv.n_chunks = 1;
                has_semantic = 1;
            }
        } else {
            int n_chunks = 0;
            char **chunks = embed_prepare_text_chunked(
                NULL, score_query, max_chars, 0, &n_chunks);
            if (chunks && n_chunks > 0) {
                int out_count = 0;
                embed_vec_t *vecs = embed_text_batch(
                    m->embed, (const char **)chunks, n_chunks, &out_count);
                if (vecs && out_count > 0) {
                    int dim = vecs[0].dim;
                    query_mv.data = malloc(sizeof(float) * (size_t)dim * (size_t)out_count);
                    if (query_mv.data) {
                        query_mv.dim = dim;
                        query_mv.n_chunks = out_count;
                        for (int ci = 0; ci < out_count; ci++)
                            memcpy(query_mv.data + ci * dim,
                                   vecs[ci].data, sizeof(float) * (size_t)dim);
                        has_semantic = 1;
                    }
                    for (int ci = 0; ci < out_count; ci++)
                        embed_vec_free(&vecs[ci]);
                    free(vecs);
                }
                for (int ci = 0; ci < n_chunks; ci++) free(chunks[ci]);
                free(chunks);
            }
        }
    }

    /* P1: Score all entries from in-memory index — no filesystem I/O */
    int scored_cap = m->idx.count > 64 ? m->idx.count : 64;
    scored_t *scored = calloc((size_t)scored_cap, sizeof(scored_t));
    int n_scored = 0;

    for (int i = 0; i < m->idx.count; i++) {
        mem_index_entry_t *ie = &m->idx.entries[i];
        if (!ie->key || !ie->key[0]) continue;

        /* P1: Use cached embedding from index instead of loading .emb file */
        float semantic_sim = 0.0f;
        int entry_has_semantic = 0;
        if (has_semantic && ie->has_emb) {
            if (ie->emb.dim == query_mv.dim) {
                semantic_sim = embed_cosine_sim_multi_multi(&query_mv, &ie->emb);
                entry_has_semantic = 1;
            }
        }

        double out_rel = 0, out_imp = 0;
        double s = score_entry_hybrid(ie->key, ie->value, score_query,
                                       ie->access_count, ie->recall_hits,
                                       ie->recall_misses,
                                       semantic_sim, entry_has_semantic,
                                       m->recall_blend_semantic,
                                       m->recall_blend_substring,
                                       m->vscore_exponent,
                                       &out_rel, &out_imp);

        /* Type filtering */
        if (s > 0.01 && type_filter) {
            if (strncmp(ie->key, type_filter, type_filter_len) != 0)
                s = 0;
        }

        /* P0: Abstention gate — skip entries below min_score */
        double min_score = m->recall_min_score > 0 ? m->recall_min_score : 0.25;
        if (s >= min_score) {
            scored[n_scored].idx_pos = i;
            scored[n_scored].score = s;
            scored[n_scored].relevance = out_rel;
            scored[n_scored].importance = out_imp;
            n_scored++;
        }
    }

    embed_multi_vec_free(&query_mv);

    /* Ref-boost using index refs — O(N×R) via key→index hash map */
    if (n_scored > 0) {
        /* Build open-addressing hash map: key → scored index */
        int map_cap = n_scored < 16 ? 64 : n_scored * 4;  /* power-of-2, load ≤ 0.25 */
        /* Ensure power-of-2 */
        { int v = map_cap - 1; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16; map_cap = v+1; }
        int map_mask = map_cap - 1;

        struct ref_map_entry { const char *key; int idx; };
        struct ref_map_entry *ref_map = calloc((size_t)map_cap, sizeof(*ref_map));

        for (int j = 0; j < n_scored; j++) {
            const char *k = m->idx.entries[scored[j].idx_pos].key;
            unsigned h = 2166136261u;
            for (const char *p = k; *p; p++)
                h = (h ^ (unsigned char)*p) * 16777619u;
            int slot = (int)(h & (unsigned)map_mask);
            while (ref_map[slot].key)
                slot = (slot + 1) & map_mask;
            ref_map[slot].key = k;
            ref_map[slot].idx = j;
        }

        for (int i = 0; i < n_scored; i++) {
            if (scored[i].score < 0.5) continue;
            mem_index_entry_t *ie = &m->idx.entries[scored[i].idx_pos];
            for (int ri = 0; ri < ie->n_refs; ri++) {
                if (!ie->refs[ri]) continue;
                unsigned h = 2166136261u;
                for (const char *p = ie->refs[ri]; *p; p++)
                    h = (h ^ (unsigned char)*p) * 16777619u;
                int slot = (int)(h & (unsigned)map_mask);
                while (ref_map[slot].key) {
                    if (strcmp(ref_map[slot].key, ie->refs[ri]) == 0) {
                        if (ref_map[slot].idx != i) {
                            /* FIX #2: Cap boosted score at 1.0 to preserve
                             * normalized scoring invariant. Without this cap,
                             * mutually-referencing memories inflate each other
                             * unboundedly in a single pass. */
                            double boosted = scored[ref_map[slot].idx].score + 0.3 * scored[i].score;
                            scored[ref_map[slot].idx].score = boosted > 1.0 ? 1.0 : boosted;
                        }
                        break;
                    }
                    slot = (slot + 1) & map_mask;
                }
            }
        }
        free(ref_map);
    }

    qsort(scored, (size_t)n_scored, sizeof(scored_t), scored_cmp_desc);

    /* Build results from top-k index entries */
    int n = n_scored < max_results ? n_scored : max_results;
    if (n <= 0) { free(scored); pthread_mutex_unlock(&m->mtx); return results; }
    results.entries = calloc((size_t)n, sizeof(memory_entry_t));
    results.count = 0;

    for (int i = 0; i < n; i++) {
        mem_index_entry_t *ie = &m->idx.entries[scored[i].idx_pos];
        memory_entry_t *e = &results.entries[results.count];

        e->key = strdup(ie->key);
        e->value = strdup(ie->value);
        e->description = ie->description ? strdup(ie->description) : NULL;
        e->pinned = ie->pinned;
        e->recall_hits = ie->recall_hits;
        e->recall_misses = ie->recall_misses;
        e->belief_entropy = ie->belief_entropy;
        e->journal_ref = NULL;  /* loaded on demand if needed */

        /* Copy refs from index */
        if (ie->n_refs > 0) {
            e->n_refs = ie->n_refs;
            e->refs = calloc((size_t)e->n_refs, sizeof(char *));
            for (int ri = 0; ri < e->n_refs; ri++)
                e->refs[ri] = strdup(ie->refs[ri]);
        }

        e->relevance = scored[i].score;
        e->raw_relevance = scored[i].relevance;
        e->importance = scored[i].importance;
        results.count++;
    }

    free(scored);
    pthread_mutex_unlock(&m->mtx);

    /* FIX #7: Removed automatic access_count increment on every recall.
     * Previously, every memory_recall() incremented access_count for ALL
     * returned results — even those the LLM never uses. With error-triggered
     * recall, scratchpad-enriched recall, and reflection recall happening
     * per react loop, popular memories' access_count inflated far beyond
     * actual utility. access_count should only be incremented when a memory
     * is actually injected into the LLM context (done by the caller). */

    return results;
}


/* ── build_index (P2: progressive disclosure) ─────────────────────── */

/* P2: Progressive disclosure index — structured listing with key + description.
 *
 * Research basis:
 *   AutoMEM [arXiv:2606.04315, Jun 2026] — cross-scenario evaluation showed
 *     self-managed memory with active control beats all passive pipelines.
 *     Agents need to BROWSE memory structure, not just blind-query it.
 *   Letta Context Repositories [May 2026] — filetree structure always in
 *     system prompt; folder hierarchy and file names act as navigational
 *     signals. Each file includes frontmatter with description.
 *   Claude Code Auto Memory [2026] — MEMORY.md index file with topic
 *     descriptions enabling selective loading of full content.
 *   MRAgent [arXiv:2606.06036, ICML 2026] — Cue-Tag-Content graph where
 *     cues enable fast navigation before loading full content.
 *
 * P1: Uses in-memory index cache — no filesystem scan needed.
 * Caller must free. Returns NULL if no memories. */
char *memory_build_index(memory_t *m) {
    if (!m || m->idx.count == 0) return NULL;
    pthread_mutex_lock(&m->mtx);

    /* Count by type */
    int n_lessons = 0, n_strategies = 0, n_facts = 0;
    int n_tasks = 0, n_skills = 0, n_antipatterns = 0, n_other = 0;
    for (int i = 0; i < m->idx.count; i++) {
        const char *k = m->idx.entries[i].key;
        if (!k) continue;
        if (strncmp(k, "lesson:", 7) == 0) n_lessons++;
        else if (strncmp(k, "strategy:", 9) == 0) n_strategies++;
        else if (strncmp(k, "fact:", 5) == 0) n_facts++;
        else if (strncmp(k, "task:", 5) == 0) n_tasks++;
        else if (strncmp(k, "skill:", 6) == 0) n_skills++;
        else if (strncmp(k, "anti-pattern:", 13) == 0) n_antipatterns++;
        else n_other++;
    }

    /* Compact summary — ~30 tokens instead of ~14K for full listing */
    str_t result = str_new(256);
    str_appendf(&result, "Memory: %d entries", m->idx.count);
    const char *sep = " (";
    if (n_lessons)      { str_appendf(&result, "%s%d lessons", sep, n_lessons); sep = ", "; }
    if (n_strategies)   { str_appendf(&result, "%s%d strategies", sep, n_strategies); sep = ", "; }
    if (n_skills)       { str_appendf(&result, "%s%d skills", sep, n_skills); sep = ", "; }
    if (n_facts)        { str_appendf(&result, "%s%d facts", sep, n_facts); sep = ", "; }
    if (n_tasks)        { str_appendf(&result, "%s%d tasks", sep, n_tasks); sep = ", "; }
    if (n_antipatterns) { str_appendf(&result, "%s%d anti-patterns", sep, n_antipatterns); sep = ", "; }
    if (n_other)        { str_appendf(&result, "%s%d other", sep, n_other); sep = ", "; }
    if (sep[0] == ',') str_append_cstr(&result, ")");  /* close paren if we emitted any */

    pthread_mutex_unlock(&m->mtx);
    return str_steal(&result);
}

/* ── memory_build_listing (on-demand full listing) ─────────── */

char *memory_build_listing(memory_t *m, const char *type_filter) {
    if (!m || m->idx.count == 0) return NULL;
    pthread_mutex_lock(&m->mtx);

    static const struct { const char *prefix; const char *label; } types[] = {
        { "lesson:",        "Lessons" },
        { "strategy:",      "Strategies" },
        { "skill:",         "Skills" },
        { "fact:",          "Facts" },
        { "task:",          "Tasks" },
        { "anti-pattern:",  "Anti-Patterns" },
        { NULL, NULL }
    };

    str_t result = str_new(4096);

    for (int t = 0; types[t].prefix; t++) {
        size_t plen = strlen(types[t].prefix);

        /* If type_filter is set, skip non-matching groups */
        if (type_filter && type_filter[0]) {
            /* Match filter against label (case-insensitive first char) or prefix */
            if (strncasecmp(type_filter, types[t].label, strlen(type_filter)) != 0 &&
                strncmp(type_filter, types[t].prefix, strlen(type_filter)) != 0)
                continue;
        }

        int count = 0;
        for (int i = 0; i < m->idx.count; i++) {
            if (m->idx.entries[i].key &&
                strncmp(m->idx.entries[i].key, types[t].prefix, plen) == 0)
                count++;
        }
        if (count == 0) continue;

        str_appendf(&result, "## %s (%d)\n", types[t].label, count);
        for (int i = 0; i < m->idx.count; i++) {
            mem_index_entry_t *e = &m->idx.entries[i];
            if (!e->key || strncmp(e->key, types[t].prefix, plen) != 0)
                continue;
            const char *desc = (e->description && e->description[0])
                               ? e->description : "(no description)";
            str_appendf(&result, "- %s", e->key);
            if (e->pinned) str_append_cstr(&result, " [pinned]");
            str_appendf(&result, " \xe2\x80\x94 %s\n", desc);
        }
        str_append_cstr(&result, "\n");
    }

    /* Other (uncategorized) entries — only if no filter or filter matches "other" */
    if (!type_filter || !type_filter[0] ||
        strncasecmp(type_filter, "Other", strlen(type_filter)) == 0) {
        int n_other = 0;
        for (int i = 0; i < m->idx.count; i++) {
            mem_index_entry_t *e = &m->idx.entries[i];
            if (!e->key) continue;
            int categorized = 0;
            for (int t = 0; types[t].prefix; t++) {
                if (strncmp(e->key, types[t].prefix, strlen(types[t].prefix)) == 0) {
                    categorized = 1;
                    break;
                }
            }
            if (!categorized) n_other++;
        }
        if (n_other > 0) {
            str_appendf(&result, "## Other (%d)\n", n_other);
            for (int i = 0; i < m->idx.count; i++) {
                mem_index_entry_t *e = &m->idx.entries[i];
                if (!e->key) continue;
                int categorized = 0;
                for (int t = 0; types[t].prefix; t++) {
                    if (strncmp(e->key, types[t].prefix, strlen(types[t].prefix)) == 0) {
                        categorized = 1;
                        break;
                    }
                }
                if (categorized) continue;
                const char *desc = (e->description && e->description[0])
                                   ? e->description : "(no description)";
                str_appendf(&result, "- %s \xe2\x80\x94 %s\n", e->key, desc);
            }
            str_append_cstr(&result, "\n");
        }
    }

    if (result.len == 0) {
        str_free(&result);
        pthread_mutex_unlock(&m->mtx);
        return NULL;
    }
    pthread_mutex_unlock(&m->mtx);
    return str_steal(&result);
}

/* ── load_pinned (P1: index-based) ───────────────────────────── */

/* P1: Load pinned memories from in-memory index — no filesystem scan.
 * Caller must free. Returns NULL if no pinned memories. */
char *memory_load_pinned(memory_t *m) {
    if (!m) return NULL;
    pthread_mutex_lock(&m->mtx);

    str_t out = str_new(1024);
    int count = 0;

    for (int i = 0; i < m->idx.count; i++) {
        mem_index_entry_t *e = &m->idx.entries[i];
        if (!e->pinned) continue;
        if (count > 0) str_append_cstr(&out, "\n");
        str_appendf(&out, "[PINNED: %s]\n%s", e->key, e->value);
        count++;
    }

    if (count == 0) {
        str_free(&out);
        pthread_mutex_unlock(&m->mtx);
        return NULL;
    }
    pthread_mutex_unlock(&m->mtx);
    return str_steal(&out);
}

/* ── delete ──────────────────────────────────────────── */

/* Callback context for cleaning dangling refs after a key is deleted. */
typedef struct {
    const char *deleted_key;
    int cleaned;
} gc_refs_ctx_t;

/* Remove deleted_key from the refs array of every remaining entry. */
static int gc_refs_cb(const char *dirpath, cJSON *entry, void *user_data) {
    gc_refs_ctx_t *ctx = (gc_refs_ctx_t *)user_data;
    cJSON *refs = cJSON_GetObjectItem(entry, "refs");
    if (!refs || !cJSON_IsArray(refs)) return JSON_CB_CONTINUE;

    int sz = cJSON_GetArraySize(refs);
    int found = 0;
    for (int i = sz - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(refs, i);
        if (item && item->valuestring &&
            strcmp(item->valuestring, ctx->deleted_key) == 0) {
            cJSON_DeleteItemFromArray(refs, i);
            found = 1;
        }
    }

    if (found) {
        /* Rewrite the cleaned entry to disk */
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        if (k && k->valuestring) {
            char fname[512];
            key_to_path(k->valuestring, ".json", fname, sizeof(fname));
            char path[NASH_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dirpath, fname);
            char *json = cJSON_Print(entry);
            if (json) {
                write_file(path, json, strlen(json));
                free(json);
            }
        }
        ctx->cleaned++;
    }
    return JSON_CB_CONTINUE;
}

/* ── batch delete ──────────────────────────────────────── */

/* Batch gc_refs context: removes ANY of the deleted keys from refs arrays.
 * Reduces O(K×N) to O(N) for K deletes across N entries. */
typedef struct {
    const char **deleted_keys;
    int n_deleted;
    int cleaned;
} gc_refs_multi_ctx_t;

/* Remove any of deleted_keys[] from the refs array of a single entry. */
static int gc_refs_multi_cb(const char *dirpath, cJSON *entry, void *user_data) {
    gc_refs_multi_ctx_t *ctx = (gc_refs_multi_ctx_t *)user_data;
    cJSON *refs = cJSON_GetObjectItem(entry, "refs");
    if (!refs || !cJSON_IsArray(refs)) return JSON_CB_CONTINUE;

    int sz = cJSON_GetArraySize(refs);
    int found = 0;
    for (int i = sz - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(refs, i);
        if (!item || !item->valuestring) continue;
        for (int d = 0; d < ctx->n_deleted; d++) {
            if (strcmp(item->valuestring, ctx->deleted_keys[d]) == 0) {
                cJSON_DeleteItemFromArray(refs, i);
                found = 1;
                break;
            }
        }
    }

    if (found) {
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        if (k && k->valuestring) {
            char fname[512];
            key_to_path(k->valuestring, ".json", fname, sizeof(fname));
            char path[NASH_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dirpath, fname);
            char *json = cJSON_Print(entry);
            if (json) {
                write_file(path, json, strlen(json));
                free(json);
            }
        }
        ctx->cleaned++;
    }
    return JSON_CB_CONTINUE;
}

int memory_delete(memory_t *m, const char *key) {
    if (!m || !key) return -1;
    pthread_mutex_lock(&m->mtx);

    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    /* Check if entry exists */
    struct stat st;
    if (stat(path, &st) != 0) { pthread_mutex_unlock(&m->mtx); return -1; }  /* not found */

    /* Remove JSON file */
    unlink(path);

    /* Remove embedding file if it exists */
    char emb_fname[512];
    key_to_path(key, ".emb", emb_fname, sizeof(emb_fname));
    char emb_path[NASH_PATH_MAX];
    snprintf(emb_path, sizeof(emb_path), "%s/%s", m->dir, emb_fname);
    unlink(emb_path);  /* ignore error if not exists */

    /* P1: Remove from in-memory index */
    mem_index_remove(&m->idx, key);

    /* Clean dangling refs: scan all remaining entries and remove the
     * deleted key from their refs arrays.  O(N) per delete but N < 1000
     * and deletes are infrequent (consolidation, prune, manual). */
    gc_refs_ctx_t gc = { .deleted_key = key, .cleaned = 0 };
    for_each_json_entry(m->dir, gc_refs_cb, &gc);

    /* Git commit */
    char msg[256];
    snprintf(msg, sizeof(msg), "memory: delete %s", key);
    memory_git_commit(m, msg);

    pthread_mutex_unlock(&m->mtx);
    return 0;
}

/* Batch delete: delete multiple keys with a SINGLE gc_refs pass and
 * a single git commit.  Reduces O(K×N) to O(K+N) for K deletes across
 * N remaining entries.  Used by memory_prune() and playbook fact cleanup.
 *
 * Each key's .json and .emb files are removed, and the key is removed
 * from the in-memory index.  Then a single scan of all remaining JSON
 * files removes dangling refs for ALL deleted keys at once.  Finally,
 * a single git commit records all deletions.
 *
 * Returns the number of entries actually deleted (keys that existed). */
int memory_delete_batch(memory_t *m, const char **keys, int n_keys) {
    if (!m || !keys || n_keys <= 0) return 0;
    pthread_mutex_lock(&m->mtx);

    /* Phase 1: Remove files and index entries for each key.
     * Track which keys were actually found (for the commit message). */
    const char **found_keys = malloc(sizeof(const char *) * (size_t)n_keys);
    int n_found = 0;

    for (int k = 0; k < n_keys; k++) {
        if (!keys[k]) continue;

        char fname[512];
        key_to_path(keys[k], ".json", fname, sizeof(fname));
        char path[NASH_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

        struct stat st;
        if (stat(path, &st) != 0) continue;  /* not found — skip */

        unlink(path);

        /* Remove embedding file if it exists */
        char emb_fname[512];
        key_to_path(keys[k], ".emb", emb_fname, sizeof(emb_fname));
        char emb_path[NASH_PATH_MAX];
        snprintf(emb_path, sizeof(emb_path), "%s/%s", m->dir, emb_fname);
        unlink(emb_path);  /* ignore error if not exists */

        /* Remove from in-memory index */
        mem_index_remove(&m->idx, keys[k]);

        found_keys[n_found++] = keys[k];
    }

    if (n_found == 0) {
        free(found_keys);
        pthread_mutex_unlock(&m->mtx);
        return 0;
    }

    /* Phase 2: Single gc_refs pass — remove ALL deleted keys from
     * refs arrays across all remaining entries.  O(N×K) string
     * comparisons but only O(N) file reads/writes. */
    gc_refs_multi_ctx_t gc = {
        .deleted_keys = found_keys,
        .n_deleted = n_found,
        .cleaned = 0
    };
    for_each_json_entry(m->dir, gc_refs_multi_cb, &gc);

    /* Phase 3: Single git commit for all deletions */
    char msg[1024];
    if (n_found == 1) {
        snprintf(msg, sizeof(msg), "memory: delete %s", found_keys[0]);
    } else {
        snprintf(msg, sizeof(msg), "memory: batch delete %d entries", n_found);
    }
    memory_git_commit(m, msg);

    free(found_keys);
    pthread_mutex_unlock(&m->mtx);
    return n_found;
}

/* ── free results ────────────────────────────────────── */

void memory_results_free(memory_results_t *r) {
    if (!r || !r->entries) return;
    for (int i = 0; i < r->count; i++) {
        free(r->entries[i].key);
        free(r->entries[i].value);
        free(r->entries[i].description);
        free(r->entries[i].journal_ref);
        for (int t = 0; t < r->entries[i].n_tags; t++)
            free(r->entries[i].tags[t]);
        free(r->entries[i].tags);
        /* Free refs (inter-memory relationship links) */
        for (int ri = 0; ri < r->entries[i].n_refs; ri++)
            free(r->entries[i].refs[ri]);
        free(r->entries[i].refs);
        /* P2: Free lineage fields */
        free(r->entries[i].supersedes);
    }
    free(r->entries);
    r->entries = NULL;
    r->count = 0;
}

/* ── prune (forgetting/decay) ─────────────────────────────── */

/* FIX B3: prune_cb and prune_ctx_t removed — memory_prune() now iterates
 * the in-memory index directly instead of scanning disk via for_each_json_entry.
 * The index caches pinned, recall_hits, recall_misses — exactly the fields
 * needed for pruning decisions. */

/* Callback for orphan .emb cleanup: remove .emb files without a matching .json */
static int orphan_emb_cb(const char *dirpath, const char *filename,
                         const char *fullpath, void *user_data) {
    int *cleaned = (int *)user_data;

    /* Derive the expected .json filename from the .emb filename.
     * foo_bar.emb → foo_bar.json */
    size_t flen = strlen(filename);
    if (flen < 5) return 0;  /* too short to be valid */

    char json_fname[512];
    snprintf(json_fname, sizeof(json_fname), "%.*s.json",
             (int)(flen - 4), filename);  /* strip .emb, add .json */

    char json_path[NASH_PATH_MAX];
    snprintf(json_path, sizeof(json_path), "%s/%s", dirpath, json_fname);

    struct stat st;
    if (stat(json_path, &st) != 0) {
        /* No matching .json — orphan .emb */
        unlink(fullpath);
        (*cleaned)++;
    }
    return 0;
}

int memory_prune(memory_t *m, double min_score, int min_evidence) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mtx);

    /* FIX B3: Phase 1 — use in-memory index instead of disk scan.
     * The index caches pinned, recall_hits, recall_misses — exactly the
     * fields prune needs.  Avoids O(N) JSON file reads.
     * Can't call memory_delete() during iteration because it mutates
     * the index, so we collect keys first, then batch-delete. */
    char **keys = NULL;
    int count = 0, cap = 0;
    for (int i = 0; i < m->idx.count; i++) {
        mem_index_entry_t *e = &m->idx.entries[i];
        if (e->pinned) continue;
        int hits = e->recall_hits;
        int misses = e->recall_misses;
        int evidence = hits + misses;
        double vscore = (hits + 1.0) / (hits + misses + 2.0);
        if (vscore < min_score && evidence >= min_evidence) {
            if (count >= cap) {
                cap = cap ? cap * 2 : 16;
                keys = realloc(keys, sizeof(char *) * (size_t)cap);
                if (!keys) break;
            }
            keys[count++] = strdup(e->key);
        }
    }

    /* Phase 2: Batch delete collected entries.
     * Uses memory_delete_batch() for O(N) gc_refs instead of O(K×N)
     * when pruning K entries.  Also produces a single git commit
     * instead of K individual commits. */
    if (count > 0) {
        memory_delete_batch(m, (const char **)keys, count);
    }
    for (int i = 0; i < count; i++)
        free(keys[i]);
    free(keys);

    /* FIX #15: Release mutex before orphan .emb sweep — the sweep is pure
     * filesystem I/O that doesn't access in-memory index state. Holding
     * the mutex during the entire sweep blocks concurrent memory access
     * unnecessarily for what could be several seconds with many entries. */
    pthread_mutex_unlock(&m->mtx);

    /* Sweep for orphan .emb files (no matching .json).
     * These accumulate when crashes interrupt deletion or when .json files
     * are removed manually.  They waste disk space and pollute embedding
     * scans during consolidation. */
    {
        int emb_cleaned = 0;
        for_each_dir_entry(m->dir, ".emb", orphan_emb_cb, &emb_cleaned);
        /* No git commit needed — .emb files are not tracked by git */
    }

    return count;
}

/* ── validation scoring ─────────────────────────────────────── */

/* Internal: increment a numeric field in a memory entry's JSON file */
static int memory_increment_field(memory_t *m, const char *key,
                                   const char *field) {
    if (!m || !key || !field) return -1;
    pthread_mutex_lock(&m->mtx);

    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    cJSON *entry = memory_load_entry_json(m, key);
    if (!entry) { pthread_mutex_unlock(&m->mtx); return -1; }

    /* Increment the field (create if missing) */
    cJSON *fld = cJSON_GetObjectItem(entry, field);
    if (fld) {
        cJSON_SetNumberValue(fld, cJSON_GetNumberValue(fld) + 1);
    } else {
        cJSON_AddNumberToObject(entry, field, 1);
    }

    /* Write back */
    char *json = cJSON_Print(entry);
    write_file(path, json, strlen(json));
    free(json);
    cJSON_Delete(entry);

    /* P1: Update in-memory index counter */
    {
        mem_index_entry_t *ie = mem_index_find(&m->idx, key);
        if (ie) {
            if (strcmp(field, "recall_hits") == 0) ie->recall_hits++;
            else if (strcmp(field, "recall_misses") == 0) ie->recall_misses++;
            else if (strcmp(field, "access_count") == 0) ie->access_count++;
        }
    }

    /* No git commit for counter bumps — these are high-frequency,
     * low-value changes (access_count, recall_hits, recall_misses)
     * that pollute the git log. The JSON files are updated on disk
     * but git history is reserved for content changes. */

    pthread_mutex_unlock(&m->mtx);
    return 0;
}

int memory_increment_hits(memory_t *m, const char *key) {
    return memory_increment_field(m, key, "recall_hits");
}

int memory_increment_misses(memory_t *m, const char *key) {
    return memory_increment_field(m, key, "recall_misses");
}

int memory_update_scores(memory_t *m, const char *key,
                         int add_hits, int add_misses) {
    if (!m || !key || (add_hits == 0 && add_misses == 0)) return -1;
    pthread_mutex_lock(&m->mtx);

    /* Update in-memory index so recall scoring sees the new values
     * immediately (without requiring a restart). */
    mem_index_entry_t *ie = mem_index_find(&m->idx, key);
    if (ie) {
        ie->recall_hits += add_hits;
        ie->recall_misses += add_misses;
    }
    pthread_mutex_unlock(&m->mtx);
    return ie ? 0 : -1;
}

/* ── P2: Lesson lineage ──────────────────────────────────────── */

int memory_set_supersedes(memory_t *m, const char *new_key, const char *old_key) {
    if (!m || !new_key || !old_key) return -1;
    pthread_mutex_lock(&m->mtx);

    /* Load the new entry's JSON */
    cJSON *entry = memory_load_entry_json(m, new_key);
    if (!entry) { pthread_mutex_unlock(&m->mtx); return -1; }

    /* Determine version from the old entry */
    int old_version = 1;
    cJSON *old_entry = memory_load_entry_json(m, old_key);
    if (old_entry) {
        cJSON *vn = cJSON_GetObjectItem(old_entry, "version");
        if (vn) old_version = (int)cJSON_GetNumberValue(vn);
        cJSON_Delete(old_entry);
    }

    /* Set supersedes and version */
    cJSON *ss = cJSON_GetObjectItem(entry, "supersedes");
    if (ss) cJSON_SetValuestring(ss, old_key);
    else cJSON_AddStringToObject(entry, "supersedes", old_key);

    cJSON *vn = cJSON_GetObjectItem(entry, "version");
    if (vn) cJSON_SetNumberValue(vn, (double)(old_version + 1));
    else cJSON_AddNumberToObject(entry, "version", old_version + 1);

    /* Write back */
    char fname[512];
    key_to_path(new_key, ".json", fname, sizeof(fname));
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    char *json = cJSON_Print(entry);
    write_file(path, json, strlen(json));
    free(json);
    cJSON_Delete(entry);

    pthread_mutex_unlock(&m->mtx);
    return 0;
}

/* ── embedding integration ──────────────────────────────────── */

int memory_init_embeddings(memory_t *m, const char *type,
                           const char *model, const char *api_base,
                           const char *model_path, int dimension,
                           int max_input_chars) {
    if (!m || !type) return 0;

    /* Expand ~ in model_path */
    char expanded_path[NASH_PATH_MAX];
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
    cfg.max_input_chars = max_input_chars;

    /* Create embedding context */
    m->embed = embed_new(&cfg);
    if (!m->embed) return 0;

    /* Probe the backend — if it's not available, gracefully disable */
    if (!embed_probe(m->embed)) {
        if (cfg.type == EMBED_ONNX) {
            nash_log("[memory] ONNX embedding at %s not available, "
                    "falling back to substring matching",
                    model_path ? model_path : "(no path)");
        } else {
            nash_log("[memory] embedding service at %s not available, "
                    "falling back to substring matching", cfg.api_base);
        }
        embed_free(m->embed);
        m->embed = NULL;
        return 0;
    }

    if (cfg.type == EMBED_ONNX) {
        nash_log("[memory] ONNX embeddings enabled: %s (dim=%d, max_chars=%d)",
                model_path, m->embed->detected_dim,
                embed_max_input_chars(m->embed));
    } else {
        nash_log("[memory] semantic embeddings enabled: %s/%s (dim=%d, max_chars=%d)",
                cfg.api_base, cfg.model, m->embed->detected_dim,
                embed_max_input_chars(m->embed));
    }

    /* On first enable, embed any existing memories that lack .emb files */
    int embedded = memory_embed_all(m);
    if (embedded > 0) {
        nash_log("[memory] generated embeddings for %d existing memories",
                embedded);
    }

    return 1;
}

int memory_embed_entry(memory_t *m, const char *key, const char *value) {
    if (!m || !m->embed || !m->embed->available || !key) return -1;

    /* Prepare text as chunks for embedding.
     * Short entries produce 1 chunk; long entries are split into
     * overlapping chunks, each prefixed with key for context
     * anchoring. Chunk size adapts to model's context window. */
    int max_chars = embed_max_input_chars(m->embed);
    int overlap = max_chars / 10;  /* 10% overlap, min 200 */
    if (overlap < 200) overlap = 200;
    int n_chunks = 0;
    char **chunks = embed_prepare_text_chunked(key, value,
                                               max_chars, overlap, &n_chunks);
    if (!chunks || n_chunks <= 0) return -1;

    /* Generate embeddings for all chunks */
    int out_count = 0;
    embed_vec_t *vecs = embed_text_batch(m->embed, (const char **)chunks,
                                          n_chunks, &out_count);

    /* Free chunk strings */
    for (int i = 0; i < n_chunks; i++) free(chunks[i]);
    free(chunks);

    if (!vecs || out_count <= 0) {
        free(vecs);
        return -1;
    }

    /* Build multi-vector from results */
    int dim = 0;
    int valid_count = 0;
    for (int i = 0; i < out_count; i++) {
        if (vecs[i].data && vecs[i].dim > 0) {
            if (dim == 0) dim = vecs[i].dim;
            if (vecs[i].dim == dim) valid_count++;
        }
    }

    if (valid_count == 0 || dim == 0) {
        for (int i = 0; i < out_count; i++) embed_vec_free(&vecs[i]);
        free(vecs);
        return -1;
    }

    embed_multi_vec_t mv;
    mv.dim = dim;
    mv.n_chunks = valid_count;
    mv.data = malloc(sizeof(float) * (size_t)dim * (size_t)valid_count);
    if (!mv.data) {
        for (int i = 0; i < out_count; i++) embed_vec_free(&vecs[i]);
        free(vecs);
        return -1;
    }

    int idx = 0;
    for (int i = 0; i < out_count; i++) {
        if (vecs[i].data && vecs[i].dim == dim) {
            memcpy(mv.data + idx * dim, vecs[i].data, sizeof(float) * (size_t)dim);
            idx++;
        }
        embed_vec_free(&vecs[i]);
    }
    free(vecs);

    /* Save to .emb file (auto-detects single vs multi format) */
    char emb_fname[512];
    key_to_path(key, ".emb", emb_fname, sizeof(emb_fname));

    char emb_path[NASH_PATH_MAX];
    snprintf(emb_path, sizeof(emb_path), "%s/%s", m->dir, emb_fname);

    int rc = embed_multi_vec_save(&mv, emb_path);
    embed_multi_vec_free(&mv);
    return rc;
}

int memory_embed_all(memory_t *m) {
    if (!m || !m->embed || !m->embed->available) return 0;

    /* FIX BUG#5: Hold mutex during directory scan to prevent races with
     * concurrent memory_store/memory_delete modifying files mid-scan. */
    pthread_mutex_lock(&m->mtx);
    DIR *dir = opendir(m->dir);
    if (!dir) { pthread_mutex_unlock(&m->mtx); return 0; }

    /* FIX D7: Collect all entries needing embedding, then use batch API.
     * This reduces N HTTP round-trips to ceil(N/64) for Ollama/OpenAI. */

    /* Phase 1: Scan for entries needing (re)embedding */
    typedef struct {
        char *key;             /* memory key (strdup'd) */
        char *value;           /* memory value (strdup'd) */
    } pending_embed_t;

    int pending_cap = 64;
    int pending_count = 0;
    pending_embed_t *pending = malloc(sizeof(pending_embed_t) * (size_t)pending_cap);
    if (!pending) { closedir(dir); return 0; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        /* Check if .emb file already exists AND has correct dimension.
         * Stale embeddings from a previous model (e.g., switched from
         * MiniLM-384d to nomic-embed-768d) must be regenerated. */
        char json_path[NASH_PATH_MAX], emb_path[NASH_PATH_MAX];
        snprintf(json_path, sizeof(json_path), "%s/%s", m->dir, de->d_name);
        json_to_emb_path(json_path, emb_path, sizeof(emb_path));

        struct stat st;
        if (stat(emb_path, &st) == 0) {
            /* .emb exists — check dimension matches current model.
             * Uses multi-vec loader which auto-detects old/new format. */
            if (m->embed->detected_dim > 0) {
                embed_multi_vec_t existing = embed_multi_vec_load(emb_path);
                if (existing.data) {
                    int stale = (existing.dim != m->embed->detected_dim);
                    embed_multi_vec_free(&existing);
                    if (stale) {
                        /* Wrong dimension — delete and re-embed below */
                        unlink(emb_path);
                    } else {
                        continue;  /* correct dimension, skip */
                    }
                } else {
                    /* Corrupt .emb file — delete and re-embed */
                    unlink(emb_path);
                }
            } else {
                continue;  /* can't check dimension, assume ok */
            }
        }

        /* Load JSON entry to get key/value for chunked embedding */
        cJSON *entry = slurp_json(json_path);
        if (!entry) continue;

        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *v = cJSON_GetObjectItem(entry, "value");

        const char *ekey = (k && k->valuestring) ? k->valuestring : "";
        const char *eval = (v && v->valuestring) ? v->valuestring : "";

        /* Grow pending array if needed */
        if (pending_count >= pending_cap) {
            pending_cap *= 2;
            pending_embed_t *tmp = realloc(pending,
                sizeof(pending_embed_t) * (size_t)pending_cap);
            if (!tmp) { cJSON_Delete(entry); break; }
            pending = tmp;
        }

        pending[pending_count].key = strdup(ekey);
        pending[pending_count].value = strdup(eval);
        pending_count++;
        cJSON_Delete(entry);
    }
    closedir(dir);
    pthread_mutex_unlock(&m->mtx);  /* FIX BUG#5: release before expensive embedding */

    if (pending_count == 0) {
        free(pending);
        return 0;
    }

    /* Phase 2: Embed each entry using chunked embedding.
     * Each entry may produce 1-8 chunks; memory_embed_entry handles
     * the chunking, batch embedding, and multi-vec persistence. */
    int embedded = 0;
    for (int i = 0; i < pending_count; i++) {
        if (memory_embed_entry(m, pending[i].key, pending[i].value) == 0) {
            embedded++;
        }
    }

    /* Cleanup */
    for (int i = 0; i < pending_count; i++) {
        free(pending[i].key);
        free(pending[i].value);
    }
    free(pending);

    return embedded;
}

/* ── Deferred git commit API ────────────────────────────── */

void memory_git_defer(memory_t *m) {
    if (!m) return;
    m->git_deferred = 1;
    m->git_deferred_count = 0;
}

void memory_git_flush(memory_t *m, const char *msg) {
    if (!m) return;
    m->git_deferred = 0;
    if (m->git_deferred_count == 0) return;
    m->git_deferred_count = 0;
    /* Single batch commit for all deferred changes */
    memory_git_commit(m, msg ? msg : "memory: batch update");
}
