/*
 * test_memory_context.c — Validate memory recall relevance for a given query.
 *
 * Usage:
 *   ./tests/test_memory_context "your query here" [--memory-dir DIR] [--threshold N]
 *
 * Loads the memory store, initializes embeddings (if configured), runs
 * memory_recall with the given query, and prints all results with their
 * relevance scores and metadata. Useful for:
 *   - Validating that the scoring formula produces sensible results
 *   - Tuning recall_min_score threshold
 *   - Checking which skills/lessons/strategies are injected for a query
 *   - Verifying that irrelevant memories are filtered out
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "memory.h"
#include "config.h"

static void print_separator(void) {
    for (int i = 0; i < 80; i++) putchar('-');
    putchar('\n');
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* When called without args (e.g., by `make test`), exit successfully.
         * This is a CLI diagnostic tool, not a unit test — no args = nothing to test. */
        printf("test_memory_context: no query specified (pass --help for usage)\n");
        return 0;
    }

    const char *query = NULL;
    const char *memory_dir = NULL;
    double threshold = 0.15;
    int max_results = 20;
    int no_embeddings = 0;
    int skills_only = 0;
    float vscore_exp = -1.0f;  /* -1 = use config default */

    /* Parse ALL arguments — options can appear before or after the query.
     * The query is the first non-option positional argument. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[++i];
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = atof(argv[++i]);
        } else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_results = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-embeddings") == 0) {
            no_embeddings = 1;
        } else if (strcmp(argv[i], "--skills-only") == 0) {
            skills_only = 1;
        } else if (strcmp(argv[i], "--vscore-exponent") == 0 && i + 1 < argc) {
            vscore_exp = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: test_memory_context [OPTIONS] \"query\"\n");
            return 0;
        } else if (!query) {
            query = argv[i];  /* first non-option arg is the query */
        }
    }

    if (!query) {
        printf("test_memory_context: no query specified (pass --help for usage)\n");
        return 0;
    }

    /* Determine memory directory */
    char mem_path[4096];
    if (memory_dir) {
        snprintf(mem_path, sizeof(mem_path), "%s", memory_dir);
    } else {
        const char *home = getenv("HOME");
        if (!home) { fprintf(stderr, "ERROR: HOME not set\n"); return 1; }
        snprintf(mem_path, sizeof(mem_path), "%s/.nash", home);
    }

    /* Load config for embedding settings */
    char config_path[4096];
    {
        const char *home = getenv("HOME");
        snprintf(config_path, sizeof(config_path), "%s/.nash/config.toml",
                 home ? home : ".");
    }
    config_t *cfg = config_load(config_path);

    /* Create memory store */
    memory_t *m = memory_new(mem_path);
    if (!m) {
        fprintf(stderr, "ERROR: Failed to create memory store at %s\n", mem_path);
        config_free(cfg);
        return 1;
    }

    /* Set threshold and scoring parameters from config */
    m->recall_min_score = threshold;
    m->recall_blend_semantic = cfg->recall_blend_semantic;
    m->recall_blend_substring = cfg->recall_blend_substring;
    m->vscore_exponent = (vscore_exp >= 0) ? vscore_exp : cfg->vscore_exponent;

    /* Initialize embeddings (unless disabled) */
    if (!no_embeddings && cfg->embedding.type &&
        strcmp(cfg->embedding.type, "none") != 0) {
        printf("Initializing embeddings: type=%s model=%s\n",
               cfg->embedding.type,
               cfg->embedding.model_path ? cfg->embedding.model_path :
               cfg->embedding.model ? cfg->embedding.model : "(default)");

        int rc = memory_init_embeddings(m,
            cfg->embedding.type,
            cfg->embedding.model ? cfg->embedding.model : NULL,
            cfg->embedding.api_base ? cfg->embedding.api_base : NULL,
            cfg->embedding.model_path ? cfg->embedding.model_path : NULL,
            cfg->embedding.dimension,
            cfg->embedding.max_input_chars);

        if (rc == 0) {
            /* memory_init_embeddings returns 0 on failure, 1 on success */
            fprintf(stderr, "WARNING: Failed to initialize embeddings, "
                    "falling back to substring matching\n");
        } else {
            printf("Embeddings initialized successfully\n");
        }
    } else {
        printf("Embeddings: disabled (substring matching only)\n");
    }

    print_separator();
    printf("Query: \"%s\"\n", query);
    printf("Threshold: %.4f\n", threshold);
    printf("Max results: %d\n", max_results);
    if (skills_only) printf("Filter: skills only\n");
    print_separator();

    /* Run memory recall */
    memory_results_t results = memory_recall(m, query, max_results);

    printf("\nResults: %d memories matched (above threshold %.4f)\n\n",
           results.count, threshold);

    /* Print results with full metadata */
    int shown = 0;
    for (int i = 0; i < results.count; i++) {
        memory_entry_t *e = &results.entries[i];

        /* Filter by type if requested */
        if (skills_only && (!e->key || strncmp(e->key, "skill:", 6) != 0))
            continue;

        shown++;

        /* Determine type from key prefix */
        const char *type = "other";
        if (e->key) {
            if (strncmp(e->key, "lesson:", 7) == 0) type = "LESSON";
            else if (strncmp(e->key, "strategy:", 9) == 0) type = "STRATEGY";
            else if (strncmp(e->key, "skill:", 6) == 0) type = "SKILL";
            else if (strncmp(e->key, "task:", 5) == 0) type = "TASK";
        }

        /* Compute Bayesian validation score for display */
        double vscore = (e->recall_hits + 1.0) /
                        (e->recall_hits + e->recall_misses + 2.0);

        printf("%2d. [%s] %s\n", shown, type, e->key ? e->key : "(no key)");
        printf("    Score: %.4f  |  rel: %.4f  |  imp: %.4f  |  vscore: %.4f  |  hits/misses: %d/%d  |  access: %d\n",
               e->relevance, e->raw_relevance, e->importance,
               vscore, e->recall_hits, e->recall_misses,
               e->access_count);

        /* tags removed */

        /* Show pinned status */
        if (e->pinned) printf("    📌 PINNED (always injected)\n");

        /* Show refs */
        if (e->n_refs > 0) {
            printf("    Refs: ");
            for (int r = 0; r < e->n_refs && r < 3; r++) {
                printf("%s%s", r > 0 ? ", " : "", e->refs[r]);
            }
            if (e->n_refs > 3) printf(", ... (%d more)", e->n_refs - 3);
            printf("\n");
        }

        /* Show value preview */
        if (e->value) {
            char preview[201];
            int vlen = (int)strlen(e->value);
            if (vlen > 200) {
                memcpy(preview, e->value, 197);
                preview[197] = '.';
                preview[198] = '.';
                preview[199] = '.';
                preview[200] = '\0';
            } else {
                memcpy(preview, e->value, vlen + 1);
            }
            /* Replace newlines with spaces for compact display */
            for (int c = 0; preview[c]; c++)
                if (preview[c] == '\n') preview[c] = ' ';
            printf("    Value: %s\n", preview);
        }
        printf("\n");
    }

    if (shown == 0) {
        printf("  (no matching memories found)\n\n");
    }

    /* Summary statistics */
    print_separator();
    printf("Summary:\n");
    printf("  Total matched: %d\n", results.count);
    printf("  Shown: %d%s\n", shown, skills_only ? " (skills only)" : "");
    if (results.count > 0) {
        printf("  Score range: %.4f - %.4f\n",
               results.entries[results.count - 1].relevance,
               results.entries[0].relevance);

        /* Count by type */
        int n_lesson = 0, n_strategy = 0, n_skill = 0, n_other = 0;
        for (int i = 0; i < results.count; i++) {
            const char *k = results.entries[i].key;
            if (k && strncmp(k, "lesson:", 7) == 0) n_lesson++;
            else if (k && strncmp(k, "strategy:", 9) == 0) n_strategy++;
            else if (k && strncmp(k, "skill:", 6) == 0) n_skill++;
            else n_other++;
        }
        printf("  By type: %d lessons, %d strategies, %d skills, %d other\n",
               n_lesson, n_strategy, n_skill, n_other);
    }
    printf("  Threshold: %.4f\n", threshold);
    print_separator();

    memory_results_free(&results);
    memory_free(m);
    config_free(cfg);
    return 0;
}
