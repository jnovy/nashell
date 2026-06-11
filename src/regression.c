/*
 * Regression testing framework for nash Self-Harness validation.
 *
 * Implementation of the validation gate from:
 *   Self-Harness [arXiv:2606.09498, Jun 2026]
 *
 * Loads YAML query banks, runs headless react_run() for each query,
 * evaluates results against typed criteria, and produces scored reports.
 */

#include "regression.h"
#include "yaml_parse.h"
#include "react.h"
#include "journal.h"
#include "tools.h"
#include "str.h"
#include "cJSON.h"
#include "nash_limits.h"
#include "nash_log.h"
#include "frontend_tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <regex.h>
#include <time.h>
#include <unistd.h>

/* Forward declaration — defined in main.c */
char *create_session_dir(const char *nash_dir) __attribute__((weak));

/* ── Criterion type parsing ─────────────────────────── */

static criterion_type_t parse_criterion_type(const char *s) {
    if (!s) return CRIT_STATUS;
    if (strcmp(s, "status") == 0)         return CRIT_STATUS;
    if (strcmp(s, "contains") == 0)       return CRIT_CONTAINS;
    if (strcmp(s, "not_contains") == 0)   return CRIT_NOT_CONTAINS;
    if (strcmp(s, "regex") == 0)          return CRIT_REGEX;
    if (strcmp(s, "tool_used") == 0)      return CRIT_TOOL_USED;
    if (strcmp(s, "tool_not_used") == 0)  return CRIT_TOOL_NOT_USED;
    if (strcmp(s, "max_steps") == 0)      return CRIT_MAX_STEPS;
    if (strcmp(s, "no_error") == 0)       return CRIT_NO_ERROR;
    if (strcmp(s, "exit_code") == 0)      return CRIT_EXIT_CODE;
    return CRIT_STATUS;
}

static const char *criterion_type_str(criterion_type_t t) {
    switch (t) {
        case CRIT_STATUS:        return "status";
        case CRIT_CONTAINS:      return "contains";
        case CRIT_NOT_CONTAINS:  return "not_contains";
        case CRIT_REGEX:         return "regex";
        case CRIT_TOOL_USED:     return "tool_used";
        case CRIT_TOOL_NOT_USED: return "tool_not_used";
        case CRIT_MAX_STEPS:     return "max_steps";
        case CRIT_NO_ERROR:      return "no_error";
        case CRIT_EXIT_CODE:     return "exit_code";
    }
    return "unknown";
}

/* ── YAML loading ───────────────────────────────────── */

static query_bank_t parse_bank(yaml_node_t *root, const char *filepath) {
    query_bank_t bank = {0};
    bank.filepath = strdup(filepath);

    const char *s;
    if ((s = yaml_str(yaml_get(root, "name"))))
        bank.name = strdup(s);
    else
        bank.name = strdup("unnamed");

    s = yaml_str(yaml_get(root, "split"));
    if (s && strcmp(s, "held-out") == 0)
        bank.split = SPLIT_HELD_OUT;
    else
        bank.split = SPLIT_HELD_IN;

    yaml_node_t *queries = yaml_get(root, "queries");
    if (!queries || queries->type != YAML_SEQUENCE) return bank;

    bank.n_queries = yaml_len(queries);
    bank.queries = calloc(bank.n_queries, sizeof(test_query_t));

    for (int i = 0; i < bank.n_queries; i++) {
        yaml_node_t *q = yaml_item(queries, i);
        if (!q) continue;

        test_query_t *tq = &bank.queries[i];
        s = yaml_str(yaml_get(q, "id"));
        tq->id = strdup(s ? s : "unnamed");

        s = yaml_str(yaml_get(q, "query"));
        tq->query = strdup(s ? s : "");

        tq->max_turns = yaml_int(yaml_get(q, "max_turns"), 0);

        yaml_node_t *criteria = yaml_get(q, "criteria");
        if (criteria && criteria->type == YAML_SEQUENCE) {
            tq->n_criteria = yaml_len(criteria);
            tq->criteria = calloc(tq->n_criteria, sizeof(criterion_t));

            for (int j = 0; j < tq->n_criteria; j++) {
                yaml_node_t *c = yaml_item(criteria, j);
                if (!c) continue;

                criterion_t *cr = &tq->criteria[j];
                cr->type = parse_criterion_type(yaml_str(yaml_get(c, "type")));
                s = yaml_str(yaml_get(c, "expect"));
                cr->expect = strdup(s ? s : "done");
                cr->weight = 1.0;

                yaml_node_t *wn = yaml_get(c, "weight");
                if (wn) {
                    const char *ws = yaml_str(wn);
                    if (ws) cr->weight = atof(ws);
                }
            }
        }
    }

    return bank;
}

query_bank_t *regression_load_banks(const char *dir, int *count) {
    *count = 0;
    DIR *d = opendir(dir);
    if (!d) return NULL;

    /* First pass: count YAML files */
    int cap = 8;
    query_bank_t *banks = calloc(cap, sizeof(query_bank_t));
    int n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 5) continue;
        if (strcmp(ent->d_name + len - 5, ".yaml") != 0 &&
            strcmp(ent->d_name + len - 4, ".yml") != 0) continue;

        char path[NASH_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        yaml_node_t *root = yaml_parse_file(path);
        if (!root) {
            fprintf(stderr, "[regression] failed to parse %s\n", path);
            continue;
        }

        if (n >= cap) {
            cap *= 2;
            banks = realloc(banks, cap * sizeof(query_bank_t));
        }

        banks[n] = parse_bank(root, path);
        yaml_free(root);
        n++;
    }
    closedir(d);

    *count = n;
    return banks;
}

void regression_free_banks(query_bank_t *banks, int count) {
    if (!banks) return;
    for (int i = 0; i < count; i++) {
        free(banks[i].name);
        free(banks[i].filepath);
        for (int j = 0; j < banks[i].n_queries; j++) {
            free(banks[i].queries[j].id);
            free(banks[i].queries[j].query);
            for (int k = 0; k < banks[i].queries[j].n_criteria; k++) {
                free(banks[i].queries[j].criteria[k].expect);
            }
            free(banks[i].queries[j].criteria);
        }
        free(banks[i].queries);
    }
    free(banks);
}

/* ── Journal analysis helpers ───────────────────────── */

typedef struct {
    int total_steps;
    int error_count;
    char **tools_used;
    int n_tools_used;
} journal_analysis_t;

static void analyze_journal(const char *journal_path, int react_loop,
                            journal_analysis_t *out) {
    memset(out, 0, sizeof(*out));

    char *data = slurp_file(journal_path, NULL);
    if (!data) return;

    int tools_cap = 32;
    out->tools_used = calloc(tools_cap, sizeof(char *));

    /* Parse line by line */
    char *line = data;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        cJSON *entry = cJSON_Parse(line);
        if (entry) {
            int rl = cJSON_GetObjectItem(entry, "react_loop")
                     ? cJSON_GetObjectItem(entry, "react_loop")->valueint : -1;

            if (rl == react_loop) {
                const char *tool = cJSON_GetStringValue(
                    cJSON_GetObjectItem(entry, "tool"));
                int step = cJSON_GetObjectItem(entry, "step")
                           ? cJSON_GetObjectItem(entry, "step")->valueint : -1;

                if (tool && strcmp(tool, "system") != 0 &&
                    strcmp(tool, "query") != 0) {
                    if (step >= out->total_steps)
                        out->total_steps = step + 1;

                    /* Check for errors */
                    cJSON *failed = cJSON_GetObjectItem(entry, "failed");
                    if (failed && cJSON_IsTrue(failed))
                        out->error_count++;
                    cJSON *err = cJSON_GetObjectItem(entry, "error");
                    if (err && cJSON_IsString(err) && err->valuestring[0])
                        out->error_count++;

                    /* Track unique tools */
                    int found = 0;
                    for (int i = 0; i < out->n_tools_used; i++) {
                        if (strcmp(out->tools_used[i], tool) == 0) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        if (out->n_tools_used >= tools_cap) {
                            tools_cap *= 2;
                            out->tools_used = realloc(out->tools_used,
                                                      tools_cap * sizeof(char *));
                        }
                        out->tools_used[out->n_tools_used++] = strdup(tool);
                    }
                }
            }
            cJSON_Delete(entry);
        }

        if (nl) line = nl + 1;
        else break;
    }

    free(data);
}

static void free_journal_analysis(journal_analysis_t *a) {
    for (int i = 0; i < a->n_tools_used; i++)
        free(a->tools_used[i]);
    free(a->tools_used);
}

/* ── Criterion evaluation ───────────────────────────── */

/* Case-insensitive substring search */
static int strcasecontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static criterion_result_t evaluate_criterion(const criterion_t *crit,
                                              const char *result,
                                              int have_result,
                                              const journal_analysis_t *ja) {
    criterion_result_t cr = {0};

    /* Build description */
    char desc[256];
    snprintf(desc, sizeof(desc), "%s(%s)", criterion_type_str(crit->type),
             crit->expect ? crit->expect : "");
    cr.criterion_desc = strdup(desc);

    switch (crit->type) {
    case CRIT_STATUS:
        if (crit->expect && strcmp(crit->expect, "done") == 0)
            cr.passed = have_result;
        else
            cr.passed = !have_result;
        if (!cr.passed)
            cr.detail = strdup(have_result ? "got done, expected fail" :
                                             "got fail, expected done");
        break;

    case CRIT_CONTAINS:
        cr.passed = result && strcasecontains(result, crit->expect);
        if (!cr.passed) {
            char buf[512];
            snprintf(buf, sizeof(buf), "result does not contain '%s'",
                     crit->expect);
            cr.detail = strdup(buf);
        }
        break;

    case CRIT_NOT_CONTAINS:
        cr.passed = !result || !strcasecontains(result, crit->expect);
        if (!cr.passed) {
            char buf[512];
            snprintf(buf, sizeof(buf), "result contains '%s' (should not)",
                     crit->expect);
            cr.detail = strdup(buf);
        }
        break;

    case CRIT_REGEX: {
        regex_t re;
        int rc = regcomp(&re, crit->expect, REG_EXTENDED | REG_NOSUB);
        if (rc == 0) {
            cr.passed = result && regexec(&re, result, 0, NULL, 0) == 0;
            regfree(&re);
        } else {
            cr.passed = 0;
        }
        if (!cr.passed)
            cr.detail = strdup("regex did not match");
        break;
    }

    case CRIT_TOOL_USED: {
        int found = 0;
        for (int i = 0; i < ja->n_tools_used; i++) {
            if (strcmp(ja->tools_used[i], crit->expect) == 0) {
                found = 1;
                break;
            }
        }
        cr.passed = found;
        if (!cr.passed) {
            char buf[256];
            snprintf(buf, sizeof(buf), "tool '%s' was not used", crit->expect);
            cr.detail = strdup(buf);
        }
        break;
    }

    case CRIT_TOOL_NOT_USED: {
        int found = 0;
        for (int i = 0; i < ja->n_tools_used; i++) {
            if (strcmp(ja->tools_used[i], crit->expect) == 0) {
                found = 1;
                break;
            }
        }
        cr.passed = !found;
        if (!cr.passed) {
            char buf[256];
            snprintf(buf, sizeof(buf), "tool '%s' was used (should not be)",
                     crit->expect);
            cr.detail = strdup(buf);
        }
        break;
    }

    case CRIT_MAX_STEPS: {
        int max = crit->expect ? atoi(crit->expect) : 20;
        cr.passed = ja->total_steps <= max;
        if (!cr.passed) {
            char buf[128];
            snprintf(buf, sizeof(buf), "used %d steps (max %d)",
                     ja->total_steps, max);
            cr.detail = strdup(buf);
        }
        break;
    }

    case CRIT_NO_ERROR:
        cr.passed = (ja->error_count == 0);
        if (!cr.passed) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d errors in journal", ja->error_count);
            cr.detail = strdup(buf);
        }
        break;

    case CRIT_EXIT_CODE: {
        int expected = crit->expect ? atoi(crit->expect) : 0;
        int actual = have_result ? 0 : 1;
        cr.passed = (actual == expected);
        if (!cr.passed) {
            char buf[128];
            snprintf(buf, sizeof(buf), "exit code %d, expected %d",
                     actual, expected);
            cr.detail = strdup(buf);
        }
        break;
    }
    }

    return cr;
}

/* ── Run regression tests ───────────────────────────── */

static query_result_t run_single_query(const test_query_t *tq,
                                        provider_t *provider,
                                        llm_config_t *llm,
                                        config_t *cfg,
                                        memory_t *memory,
                                        store_t *store,
                                        const char *nash_dir) {
    query_result_t qr = {0};
    qr.query_id = strdup(tq->id);

    fprintf(stderr, "  [regression] running: %s ... ", tq->id);

    /* Create a fresh session for this test */
    journal_t *journal = journal_new_lazy(nash_dir);

    tool_ctx_t tools = {
        .store = store,
        .journal = journal,
        .memory = memory,
        .session_dir = NULL,
        .scratchpad = NULL,
        .cfg = cfg,
        .llm = llm,
        .provider = provider,
        .react_loop = 0,
        .aliases = alias_map_new(),
    };
    scratchpad_init(&tools.scratch);

    react_flags_t flags = REACT_FLAGS_DEFAULT;
    /* Disable reflection/scoring in regression mode — we don't want
     * regression runs to mutate memory entries or trigger side effects */
    flags.enable_reflection = 0;
    flags.enable_scoring = 0;

    react_ctx_t react = {
        .provider = provider,
        .llm = llm,
        .tools = &tools,
        .max_steps = tq->max_turns > 0 ? tq->max_turns : cfg->max_react_steps,
        .verbose = 0,
        .flags = flags,
        .parent_loop = -1,
    };

    char *result = react_run(&react, tq->query, tui_on_event, NULL);
    int have_result = (result != NULL);

    /* Get journal path for analysis */
    const char *session_dir = journal_session_dir(journal);
    journal_analysis_t ja = {0};
    if (session_dir) {
        char jpath[NASH_PATH_MAX];
        snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);
        analyze_journal(jpath, 0, &ja);
    }

    qr.steps_used = ja.total_steps;
    qr.result_text = result ? strdup(result) : NULL;

    /* Evaluate all criteria */
    qr.n_crit_results = tq->n_criteria;
    qr.crit_results = calloc(tq->n_criteria, sizeof(criterion_result_t));

    double total_weight = 0;
    double passed_weight = 0;
    qr.passed = 1;

    for (int i = 0; i < tq->n_criteria; i++) {
        qr.crit_results[i] = evaluate_criterion(&tq->criteria[i],
                                                  result, have_result, &ja);
        total_weight += tq->criteria[i].weight;
        if (qr.crit_results[i].passed)
            passed_weight += tq->criteria[i].weight;
        else
            qr.passed = 0;
    }

    qr.score = total_weight > 0 ? passed_weight / total_weight : 1.0;

    fprintf(stderr, "%s (%.0f%%, %d steps)\n",
            qr.passed ? "PASS" : "FAIL",
            qr.score * 100, qr.steps_used);

    /* Cleanup */
    free_journal_analysis(&ja);
    free(result);
    alias_map_free(tools.aliases);
    scratchpad_free(&tools.scratch);
    free(tools.scratchpad);
    journal_free(journal);

    return qr;
}

regression_report_t *regression_run(query_bank_t *banks, int n_banks,
                                     int split_filter,
                                     provider_t *provider,
                                     llm_config_t *llm,
                                     config_t *cfg,
                                     memory_t *memory,
                                     store_t *store,
                                     const char *nash_dir) {
    regression_report_t *report = calloc(1, sizeof(regression_report_t));
    report->held_in_score = -1;
    report->held_out_score = -1;

    /* Count banks matching filter */
    int active_banks = 0;
    for (int i = 0; i < n_banks; i++) {
        if (split_filter >= 0 && (int)banks[i].split != split_filter) continue;
        active_banks++;
    }

    report->bank_results = calloc(active_banks, sizeof(bank_result_t));
    report->n_bank_results = 0;

    double hi_total = 0, hi_weight = 0;
    double ho_total = 0, ho_weight = 0;

    for (int i = 0; i < n_banks; i++) {
        if (split_filter >= 0 && (int)banks[i].split != split_filter) continue;

        query_bank_t *bank = &banks[i];
        bank_result_t *br = &report->bank_results[report->n_bank_results++];

        br->bank_name = strdup(bank->name);
        br->split = bank->split;
        br->total = bank->n_queries;
        br->results = calloc(bank->n_queries, sizeof(query_result_t));
        br->n_results = bank->n_queries;

        fprintf(stderr, "\n[regression] bank: %s (split: %s, %d queries)\n",
                bank->name,
                bank->split == SPLIT_HELD_IN ? "held-in" : "held-out",
                bank->n_queries);

        double bank_score = 0;
        for (int j = 0; j < bank->n_queries; j++) {
            br->results[j] = run_single_query(&bank->queries[j],
                                               provider, llm, cfg,
                                               memory, store, nash_dir);
            if (br->results[j].passed) br->passed++;
            bank_score += br->results[j].score;
            report->total_queries++;
            if (br->results[j].passed) report->total_passed++;
        }

        br->score = bank->n_queries > 0 ? bank_score / bank->n_queries : 0;

        /* Accumulate per-split scores */
        if (bank->split == SPLIT_HELD_IN) {
            hi_total += bank_score;
            hi_weight += bank->n_queries;
        } else {
            ho_total += bank_score;
            ho_weight += bank->n_queries;
        }
    }

    /* Compute split scores */
    if (hi_weight > 0) report->held_in_score = hi_total / hi_weight;
    if (ho_weight > 0) report->held_out_score = ho_total / ho_weight;

    double total_weight = hi_weight + ho_weight;
    report->overall_score = total_weight > 0
        ? (hi_total + ho_total) / total_weight : 0;

    return report;
}

/* ── Report printing ────────────────────────────────── */

void regression_print_report(const regression_report_t *report) {
    fprintf(stderr, "\n╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║       Nash Regression Test Report        ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════╝\n\n");

    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        fprintf(stderr, "  Bank: %s (%s)\n", br->bank_name,
                br->split == SPLIT_HELD_IN ? "held-in" : "held-out");
        fprintf(stderr, "  Score: %.1f%%  (%d/%d passed)\n",
                br->score * 100, br->passed, br->total);

        for (int j = 0; j < br->n_results; j++) {
            query_result_t *qr = &br->results[j];
            fprintf(stderr, "    %s %s (%.0f%%, %d steps)\n",
                    qr->passed ? "✓" : "✗",
                    qr->query_id, qr->score * 100, qr->steps_used);

            /* Show failed criteria */
            if (!qr->passed) {
                for (int k = 0; k < qr->n_crit_results; k++) {
                    if (!qr->crit_results[k].passed) {
                        fprintf(stderr, "      ✗ %s: %s\n",
                                qr->crit_results[k].criterion_desc,
                                qr->crit_results[k].detail ?
                                    qr->crit_results[k].detail : "failed");
                    }
                }
            }
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "  Overall: %.1f%%  (%d/%d passed)\n",
            report->overall_score * 100,
            report->total_passed, report->total_queries);

    if (report->held_in_score >= 0)
        fprintf(stderr, "  Held-in:  %.1f%%\n", report->held_in_score * 100);
    if (report->held_out_score >= 0)
        fprintf(stderr, "  Held-out: %.1f%%\n", report->held_out_score * 100);

    fprintf(stderr, "\n");
}

/* ── Report serialization ───────────────────────────── */

int regression_save_report(const regression_report_t *report, const char *path) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "overall_score", report->overall_score);
    cJSON_AddNumberToObject(root, "held_in_score", report->held_in_score);
    cJSON_AddNumberToObject(root, "held_out_score", report->held_out_score);
    cJSON_AddNumberToObject(root, "total_passed", report->total_passed);
    cJSON_AddNumberToObject(root, "total_queries", report->total_queries);

    /* Per-query results for detailed comparison */
    cJSON *banks = cJSON_AddArrayToObject(root, "banks");
    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name", br->bank_name);
        cJSON_AddStringToObject(b, "split",
            br->split == SPLIT_HELD_IN ? "held-in" : "held-out");
        cJSON_AddNumberToObject(b, "score", br->score);
        cJSON_AddNumberToObject(b, "passed", br->passed);
        cJSON_AddNumberToObject(b, "total", br->total);

        cJSON *queries = cJSON_AddArrayToObject(b, "queries");
        for (int j = 0; j < br->n_results; j++) {
            query_result_t *qr = &br->results[j];
            cJSON *q = cJSON_CreateObject();
            cJSON_AddStringToObject(q, "id", qr->query_id);
            cJSON_AddBoolToObject(q, "passed", qr->passed);
            cJSON_AddNumberToObject(q, "score", qr->score);
            cJSON_AddNumberToObject(q, "steps", qr->steps_used);
            cJSON_AddItemToArray(queries, q);
        }

        cJSON_AddItemToArray(banks, b);
    }

    char *json = cJSON_Print(root);
    int rc = write_file(path, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return rc;
}

regression_report_t *regression_load_report(const char *path) {
    char *data = slurp_file(path, NULL);
    if (!data) return NULL;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return NULL;

    regression_report_t *report = calloc(1, sizeof(regression_report_t));
    report->overall_score = cJSON_GetObjectItem(root, "overall_score")
                            ? cJSON_GetObjectItem(root, "overall_score")->valuedouble : 0;
    report->held_in_score = cJSON_GetObjectItem(root, "held_in_score")
                            ? cJSON_GetObjectItem(root, "held_in_score")->valuedouble : -1;
    report->held_out_score = cJSON_GetObjectItem(root, "held_out_score")
                             ? cJSON_GetObjectItem(root, "held_out_score")->valuedouble : -1;
    report->total_passed = cJSON_GetObjectItem(root, "total_passed")
                           ? cJSON_GetObjectItem(root, "total_passed")->valueint : 0;
    report->total_queries = cJSON_GetObjectItem(root, "total_queries")
                            ? cJSON_GetObjectItem(root, "total_queries")->valueint : 0;

    cJSON *banks = cJSON_GetObjectItem(root, "banks");
    if (banks && cJSON_IsArray(banks)) {
        report->n_bank_results = cJSON_GetArraySize(banks);
        report->bank_results = calloc(report->n_bank_results, sizeof(bank_result_t));

        for (int i = 0; i < report->n_bank_results; i++) {
            cJSON *b = cJSON_GetArrayItem(banks, i);
            bank_result_t *br = &report->bank_results[i];

            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(b, "name"));
            br->bank_name = strdup(name ? name : "");

            const char *split = cJSON_GetStringValue(cJSON_GetObjectItem(b, "split"));
            br->split = (split && strcmp(split, "held-out") == 0)
                        ? SPLIT_HELD_OUT : SPLIT_HELD_IN;

            br->score = cJSON_GetObjectItem(b, "score")
                        ? cJSON_GetObjectItem(b, "score")->valuedouble : 0;
            br->passed = cJSON_GetObjectItem(b, "passed")
                         ? cJSON_GetObjectItem(b, "passed")->valueint : 0;
            br->total = cJSON_GetObjectItem(b, "total")
                        ? cJSON_GetObjectItem(b, "total")->valueint : 0;

            cJSON *queries = cJSON_GetObjectItem(b, "queries");
            if (queries && cJSON_IsArray(queries)) {
                br->n_results = cJSON_GetArraySize(queries);
                br->results = calloc(br->n_results, sizeof(query_result_t));
                for (int j = 0; j < br->n_results; j++) {
                    cJSON *q = cJSON_GetArrayItem(queries, j);
                    query_result_t *qr = &br->results[j];
                    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(q, "id"));
                    qr->query_id = strdup(id ? id : "");
                    qr->passed = cJSON_IsTrue(cJSON_GetObjectItem(q, "passed"));
                    qr->score = cJSON_GetObjectItem(q, "score")
                                ? cJSON_GetObjectItem(q, "score")->valuedouble : 0;
                    qr->steps_used = cJSON_GetObjectItem(q, "steps")
                                     ? cJSON_GetObjectItem(q, "steps")->valueint : 0;
                }
            }
        }
    }

    cJSON_Delete(root);
    return report;
}

/* ── Validation gate ────────────────────────────────── */

validate_result_t regression_compare(const regression_report_t *baseline,
                                      const regression_report_t *current) {
    if (!baseline || !current) return VALIDATE_INCONCLUSIVE;

    fprintf(stderr, "\n╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║     Self-Harness Validation Gate         ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════╝\n\n");

    /* Compute deltas */
    double d_overall = current->overall_score - baseline->overall_score;

    fprintf(stderr, "  Overall: %.1f%% → %.1f%%  (Δ = %+.1f%%)\n",
            baseline->overall_score * 100, current->overall_score * 100,
            d_overall * 100);

    double d_in = 0, d_out = 0;
    int have_in = 0, have_out = 0;

    if (baseline->held_in_score >= 0 && current->held_in_score >= 0) {
        d_in = current->held_in_score - baseline->held_in_score;
        have_in = 1;
        fprintf(stderr, "  Held-in: %.1f%% → %.1f%%  (Δ = %+.1f%%)\n",
                baseline->held_in_score * 100, current->held_in_score * 100,
                d_in * 100);
    }

    if (baseline->held_out_score >= 0 && current->held_out_score >= 0) {
        d_out = current->held_out_score - baseline->held_out_score;
        have_out = 1;
        fprintf(stderr, "  Held-out: %.1f%% → %.1f%%  (Δ = %+.1f%%)\n",
                baseline->held_out_score * 100, current->held_out_score * 100,
                d_out * 100);
    }

    /* Show per-query changes */
    fprintf(stderr, "\n  Per-query changes:\n");
    for (int i = 0; i < current->n_bank_results; i++) {
        bank_result_t *cb = &current->bank_results[i];

        /* Find matching baseline bank */
        bank_result_t *bb = NULL;
        for (int j = 0; j < baseline->n_bank_results; j++) {
            if (strcmp(baseline->bank_results[j].bank_name, cb->bank_name) == 0) {
                bb = &baseline->bank_results[j];
                break;
            }
        }

        for (int j = 0; j < cb->n_results; j++) {
            query_result_t *cq = &cb->results[j];

            /* Find matching baseline query */
            query_result_t *bq = NULL;
            if (bb) {
                for (int k = 0; k < bb->n_results; k++) {
                    if (strcmp(bb->results[k].query_id, cq->query_id) == 0) {
                        bq = &bb->results[k];
                        break;
                    }
                }
            }

            if (bq) {
                const char *arrow;
                if (cq->passed && !bq->passed) arrow = "✗→✓";
                else if (!cq->passed && bq->passed) arrow = "✓→✗";
                else if (cq->passed) arrow = "✓→✓";
                else arrow = "✗→✗";

                double dq = cq->score - bq->score;
                if (dq != 0) {
                    fprintf(stderr, "    %s %s (%+.0f%%)\n",
                            arrow, cq->query_id, dq * 100);
                }
            } else {
                fprintf(stderr, "    NEW %s %s (%.0f%%)\n",
                        cq->passed ? "✓" : "✗",
                        cq->query_id, cq->score * 100);
            }
        }
    }

    fprintf(stderr, "\n");

    /* Apply acceptance rule:
     * Δ_in ≥ 0 AND Δ_ho ≥ 0 AND max(Δ_in, Δ_ho) > 0
     * With fallback to overall delta when only one split exists */
    validate_result_t result;

    if (have_in && have_out) {
        /* Full acceptance rule */
        double max_d = d_in > d_out ? d_in : d_out;
        if (d_in >= -0.001 && d_out >= -0.001 && max_d > 0.001) {
            result = VALIDATE_ACCEPTED;
        } else if (d_in < -0.001 || d_out < -0.001) {
            result = VALIDATE_REJECTED;
        } else {
            result = VALIDATE_INCONCLUSIVE;
        }
    } else if (have_in || have_out) {
        /* Single split: use that delta */
        double d = have_in ? d_in : d_out;
        if (d > 0.001) result = VALIDATE_ACCEPTED;
        else if (d < -0.001) result = VALIDATE_REJECTED;
        else result = VALIDATE_INCONCLUSIVE;
    } else {
        /* No per-split data: use overall */
        if (d_overall > 0.001) result = VALIDATE_ACCEPTED;
        else if (d_overall < -0.001) result = VALIDATE_REJECTED;
        else result = VALIDATE_INCONCLUSIVE;
    }

    const char *label;
    switch (result) {
        case VALIDATE_ACCEPTED:     label = "✓ ACCEPTED — harness change improves performance"; break;
        case VALIDATE_REJECTED:     label = "✗ REJECTED — harness change degrades performance"; break;
        case VALIDATE_INCONCLUSIVE: label = "? INCONCLUSIVE — no significant change"; break;
    }

    fprintf(stderr, "  Result: %s\n\n", label);

    return result;
}

/* ── Free ────────────────────────────────────────────── */

void regression_free_report(regression_report_t *report) {
    if (!report) return;
    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        free(br->bank_name);
        for (int j = 0; j < br->n_results; j++) {
            query_result_t *qr = &br->results[j];
            free(qr->query_id);
            free(qr->result_text);
            for (int k = 0; k < qr->n_crit_results; k++) {
                free(qr->crit_results[k].criterion_desc);
                free(qr->crit_results[k].detail);
            }
            free(qr->crit_results);
        }
        free(br->results);
    }
    free(report->bank_results);
    free(report);
}

/* ── Seed query bank ────────────────────────────────── */

static const char *SEED_HELD_IN_YAML =
"name: basic-held-in\n"
"split: held-in\n"
"queries:\n"
"  - id: arithmetic\n"
"    query: \"What is 2+2? Answer with just the number.\"\n"
"    max_turns: 5\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: contains\n"
"        expect: \"4\"\n"
"\n"
"  - id: file-read\n"
"    query: \"Read /etc/hostname and tell me the hostname.\"\n"
"    max_turns: 10\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: tool_used\n"
"        expect: file_read\n"
"\n"
"  - id: shell-command\n"
"    query: \"Run 'uname -r' and tell me the kernel version.\"\n"
"    max_turns: 10\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: tool_used\n"
"        expect: shell_exec\n"
"\n"
"  - id: file-write-read\n"
"    query: \"Write 'hello regression' to /tmp/nash-regression-test.txt, then read it back.\"\n"
"    max_turns: 15\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: tool_used\n"
"        expect: file_write\n"
"      - type: tool_used\n"
"        expect: file_read\n"
"      - type: contains\n"
"        expect: \"hello regression\"\n"
"\n"
"  - id: multi-step-reasoning\n"
"    query: \"List all .c files in src/, count how many there are, and report the count.\"\n"
"    max_turns: 15\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: tool_used\n"
"        expect: shell_exec\n"
"\n"
"  - id: done-signal\n"
"    query: \"Say 'task complete' and call done.\"\n"
"    max_turns: 5\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: max_steps\n"
"        expect: \"3\"\n";

static const char *SEED_HELD_OUT_YAML =
"name: basic-held-out\n"
"split: held-out\n"
"queries:\n"
"  - id: grep-search\n"
"    query: \"Search for 'react_run' in the src/ directory.\"\n"
"    max_turns: 10\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: tool_used\n"
"        expect: grep_search\n"
"\n"
"  - id: directory-listing\n"
"    query: \"List the contents of /tmp/ directory.\"\n"
"    max_turns: 10\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"\n"
"  - id: error-recovery\n"
"    query: \"Read the file /nonexistent/path/file.txt. If it doesn't exist, say 'file not found'.\"\n"
"    max_turns: 10\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: contains\n"
"        expect: \"not found\"\n"
"\n"
"  - id: multi-tool-chain\n"
"    query: \"Use the file_write tool to create /tmp/nash-chain-test.txt with content 'step1', then use grep_search to find 'step1' in /tmp/.\"\n"
"    max_turns: 15\n"
"    criteria:\n"
"      - type: status\n"
"        expect: done\n"
"      - type: tool_used\n"
"        expect: file_write\n"
"      - type: tool_used\n"
"        expect: grep_search\n";

int regression_write_seed(const char *regression_dir) {
    mkdir(regression_dir, 0755);

    /* Check if files already exist (idempotent) */
    char path[NASH_PATH_MAX];

    snprintf(path, sizeof(path), "%s/basic-held-in.yaml", regression_dir);
    if (access(path, F_OK) != 0) {
        if (write_file(path, SEED_HELD_IN_YAML, strlen(SEED_HELD_IN_YAML)) != 0)
            return -1;
        fprintf(stderr, "[regression] created seed: %s\n", path);
    }

    snprintf(path, sizeof(path), "%s/basic-held-out.yaml", regression_dir);
    if (access(path, F_OK) != 0) {
        if (write_file(path, SEED_HELD_OUT_YAML, strlen(SEED_HELD_OUT_YAML)) != 0)
            return -1;
        fprintf(stderr, "[regression] created seed: %s\n", path);
    }

    return 0;
}
