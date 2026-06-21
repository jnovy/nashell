/*
 * GEPA-inspired prompt optimization for Nash.
 *
 * Implements an iterative optimization loop connecting the existing
 * regression scorer to an LLM reflection step that rewrites
 * system_prompt_extra.
 *
 * Based on: DSPy GEPA optimizer (Stanford NLP, 2025-2026)
 *   - Separate reflection LM proposes improved instructions
 *   - Textual feedback from metric failures guides the reflection
 *   - Budget control: light (3), medium (6), heavy (10) rounds
 *   - Held-in/held-out split prevents overfitting (Self-Harness gate)
 */

#include "prompt_optimize.h"
#include "str.h"
#include "cJSON.h"
#include "nash_limits.h"
#include "nash_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Forward declarations for helpers in react.c ────── */

/* extract_llm_text_output is static in react.c, so we reimplement
 * a minimal version here for the reflection response. */
static char *extract_text_from_response(const char *raw) {
    if (!raw || !raw[0]) return NULL;

    /* Skip leading whitespace */
    raw = skip_whitespace(raw);
    if (!*raw) return NULL;

    /* Case 1: JSON with "content" field */
    if (raw[0] == '{') {
        cJSON *j = cJSON_Parse(raw);
        if (j) {
            cJSON *c = cJSON_GetObjectItem(j, "content");
            if (c && cJSON_IsString(c) && c->valuestring && c->valuestring[0]) {
                char *result = strdup(c->valuestring);
                cJSON_Delete(j);
                return result;
            }
            /* Check for "result" field (done action) */
            cJSON *r = cJSON_GetObjectItem(j, "result");
            if (r && cJSON_IsString(r) && r->valuestring && r->valuestring[0]) {
                char *result = strdup(r->valuestring);
                cJSON_Delete(j);
                return result;
            }
            /* Check for "thought" field as fallback */
            cJSON *t = cJSON_GetObjectItem(j, "thought");
            if (t && cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
                char *result = strdup(t->valuestring);
                cJSON_Delete(j);
                return result;
            }
            cJSON_Delete(j);
        }
    }

    /* Case 2: Code-fenced output — strip ``` wrapper */
    if (strncmp(raw, "```", 3) == 0) {
        const char *start = raw + 3;
        while (*start && *start != '\n') start++;
        if (*start == '\n') start++;
        const char *end = strstr(start, "\n```");
        if (end) {
            return strndup(start, end - start);
        }
    }

    /* Case 3: Plain text */
    return strdup(raw);
}

/* ── Phase 1: Textual Feedback from Failures ────────── */

char *optimize_format_feedback(const regression_report_t *report) {
    if (!report) return NULL;

    str_t fb = str_new(4096);

    str_appendf(&fb, "REGRESSION TEST RESULTS: %.1f%% overall (%d/%d passed)\n\n",
                report->overall_score * 100,
                report->total_passed, report->total_queries);

    if (report->held_in_score >= 0)
        str_appendf(&fb, "Held-in score:  %.1f%%\n", report->held_in_score * 100);
    if (report->held_out_score >= 0)
        str_appendf(&fb, "Held-out score: %.1f%%\n", report->held_out_score * 100);

    str_append_cstr(&fb, "\n");

    int has_failures = 0;

    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];

        for (int j = 0; j < br->n_results; j++) {
            query_result_t *qr = &br->results[j];

            if (qr->passed) {
                /* Brief mention of passes */
                str_appendf(&fb, "✓ PASS: %s (%.0f%%, %d steps)\n",
                            qr->query_id, qr->score * 100, qr->steps_used);
                continue;
            }

            has_failures = 1;

            str_appendf(&fb, "\n✗ FAIL: %s (score: %.0f%%, %d steps)\n",
                        qr->query_id, qr->score * 100, qr->steps_used);

            /* Show each failed criterion with detail */
            for (int k = 0; k < qr->n_crit_results; k++) {
                criterion_result_t *cr = &qr->crit_results[k];
                if (cr->passed) {
                    str_appendf(&fb, "  ✓ %s\n", cr->criterion_desc);
                } else {
                    str_appendf(&fb, "  ✗ %s: %s\n",
                                cr->criterion_desc,
                                cr->detail ? cr->detail : "failed");
                }
            }

            /* Show truncated result text for context */
            if (qr->result_text) {
                int rlen = (int)strlen(qr->result_text);
                int show = rlen > 300 ? 300 : rlen;
                str_appendf(&fb, "  Agent result (first %d chars): %.*s%s\n",
                            show, show, qr->result_text,
                            rlen > 300 ? "..." : "");
            } else {
                str_append_cstr(&fb, "  Agent result: (none — task did not complete)\n");
            }
        }
    }

    if (!has_failures) {
        str_append_cstr(&fb, "\nAll tests passed. No failures to address.\n");
    }

    return str_steal(&fb);
}

/* ── Phase 2: Reflection LM Prompt Rewriter ─────────── */

static const char *REFLECTION_SYSTEM_PROMPT =
    "You are an expert prompt engineer optimizing system instructions for an "
    "autonomous coding agent called Nash. Nash uses a ReAct loop with tools "
    "(file_read, file_write, file_edit, shell_exec, grep_search, glob_search, "
    "web_fetch, web_search, memory_store, memory_search, notes, done, plan, user_ask).\n\n"
    "Your task: given the current system prompt rules and regression test results, "
    "propose IMPROVED instructions that will make the agent pass more tests.\n\n"
    "Guidelines:\n"
    "- Focus on rules that prevent observed failure modes\n"
    "- Remove instructions that seem to cause confusion or overconstraint\n"
    "- Make implicit requirements explicit\n"
    "- Keep rules concise and actionable\n"
    "- Do NOT include tool definitions or general agent behavior — only model-specific rules\n"
    "- The output will be injected as [MODEL-SPECIFIC RULES] in the system prompt\n"
    "- Output ONLY the new prompt text, no explanations or commentary\n"
    "- Keep it under 500 words\n";

char *optimize_reflect(provider_t *reflection_lm,
                       const char *current_prompt,
                       const char *feedback_summary,
                       int round, int max_rounds) {
    if (!reflection_lm || !feedback_summary) return NULL;

    llm_chat_t *chat = llm_chat_new();

    /* System prompt for the reflection LM */
    llm_chat_add(chat, "system", REFLECTION_SYSTEM_PROMPT);

    /* User message with current state and feedback */
    str_t user_msg = str_new(8192);

    str_appendf(&user_msg, "OPTIMIZATION ROUND %d of %d\n\n", round, max_rounds);

    str_append_cstr(&user_msg, "CURRENT SYSTEM PROMPT RULES:\n");
    if (current_prompt && current_prompt[0]) {
        str_appendf(&user_msg, "---\n%s\n---\n\n", current_prompt);
    } else {
        str_append_cstr(&user_msg, "---\n(empty — no model-specific rules yet)\n---\n\n");
    }

    str_appendf(&user_msg, "%s\n", feedback_summary);

    if (round > 1) {
        str_append_cstr(&user_msg,
            "\nThis is round ");
        str_appendf(&user_msg, "%d", round);
        str_append_cstr(&user_msg,
            " — previous rounds already attempted fixes. "
            "Try a DIFFERENT approach: restructure rules, remove underperforming "
            "instructions, or add rules targeting different failure modes.\n");
    }

    str_append_cstr(&user_msg,
        "\nBased on the test results above, propose improved system prompt rules. "
        "Output ONLY the new prompt text.\n");

    llm_chat_add(chat, "user", str_cstr(&user_msg));
    str_free(&user_msg);

    /* Call the reflection LM */
    llm_stats_t stats = {0};
    char *raw_response = provider_complete(reflection_lm, chat, &stats);

    llm_chat_free(chat);

    if (!raw_response) {
        fprintf(stderr, "[optimize] reflection LM returned no response\n");
        return NULL;
    }

    /* Extract text from response */
    char *text = extract_text_from_response(raw_response);
    free(raw_response);

    if (!text) {
        fprintf(stderr, "[optimize] failed to extract text from reflection response\n");
        return NULL;
    }

    /* Trim leading/trailing whitespace */
    char *start = (char *)skip_whitespace(text);
    rtrim_whitespace(start);

    char *result = strdup(start);
    free(text);

    fprintf(stderr, "[optimize] reflection LM proposed %zu char prompt (round %d)\n",
            strlen(result), round);

    return result;
}

/* ── Phase 3: The Optimization Loop ─────────────────── */

/* Score a prompt: temporarily set system_prompt_extra, run regression, return score.
 * If out_report is non-NULL, stores the report for feedback extraction (caller frees). */
static prompt_candidate_t score_prompt(const char *prompt_text,
                                        int round,
                                        optimize_config_t *opt,
                                        query_bank_t *banks, int n_banks,
                                        config_t *cfg,
                                        memory_t *memory,
                                        store_t *store,
                                        const char *nash_dir,
                                        regression_report_t **out_report) {
    prompt_candidate_t cand = {0};
    cand.prompt_text = prompt_text ? strdup(prompt_text) : NULL;
    cand.round = round;

    if (out_report) *out_report = NULL;

    /* Temporarily override system_prompt_extra */
    const char *saved_extra = cfg->system_prompt_extra;
    cfg->system_prompt_extra = prompt_text;

    /* Run regression */
    regression_report_t *report = regression_run(
        banks, n_banks, opt->split_filter,
        opt->student, cfg, memory, store, nash_dir);

    /* Restore original */
    cfg->system_prompt_extra = saved_extra;

    if (report) {
        cand.score = report->overall_score;
        cand.held_in_score = report->held_in_score;
        cand.held_out_score = report->held_out_score;
        cand.total_passed = report->total_passed;
        cand.total_queries = report->total_queries;

        if (out_report)
            *out_report = report;  /* caller takes ownership */
        else
            regression_free_report(report);
    }

    return cand;
}

/* Check if candidate is better than current best using Self-Harness acceptance rule:
 * Must not regress on either split, and must improve on at least one. */
static int is_better(const prompt_candidate_t *candidate,
                     const prompt_candidate_t *best) {
    /* Simple case: strictly higher overall score */
    if (candidate->score > best->score + 0.001)
        return 1;

    /* Tie-breaking: if overall is similar, check splits */
    if (candidate->score >= best->score - 0.001) {
        /* Check Self-Harness acceptance rule on splits */
        if (candidate->held_in_score >= 0 && best->held_in_score >= 0 &&
            candidate->held_out_score >= 0 && best->held_out_score >= 0) {
            double d_in  = candidate->held_in_score  - best->held_in_score;
            double d_out = candidate->held_out_score - best->held_out_score;
            /* Accept if neither split regresses and at least one improves */
            if (d_in >= -0.001 && d_out >= -0.001 &&
                (d_in > 0.001 || d_out > 0.001))
                return 1;
        }

        /* Tie-break on pass count */
        if (candidate->total_passed > best->total_passed)
            return 1;
    }

    return 0;
}

/* Write winning prompt to model profile .toml */
static int write_prompt_to_profile(const char *profile_path,
                                    const char *prompt_text) {
    if (!profile_path || !prompt_text) return -1;

    char *data = slurp_file(profile_path, NULL);
    if (!data) {
        fprintf(stderr, "[optimize] cannot read profile: %s\n", profile_path);
        return -1;
    }

    str_t out = str_new(strlen(data) + strlen(prompt_text) + 256);

    /* Strategy: find existing system_prompt_extra and replace it,
     * or append if not found. */
    char *spe = strstr(data, "system_prompt_extra");
    if (spe) {
        /* Write everything before system_prompt_extra */
        str_append(&out, data, spe - data);

        /* Write the new value */
        str_append_cstr(&out, "system_prompt_extra = \"\"\"\n");
        str_append_cstr(&out, prompt_text);
        if (prompt_text[strlen(prompt_text) - 1] != '\n')
            str_append_cstr(&out, "\n");
        str_append_cstr(&out, "\"\"\"\n");

        /* Skip past the old value:
         * Find the end of the old system_prompt_extra value */
        char *val_start = spe + strlen("system_prompt_extra");
        /* Skip whitespace and = */
        while (*val_start == ' ' || *val_start == '=') val_start++;

        if (strncmp(val_start, "\"\"\"", 3) == 0) {
            /* Multi-line string: find closing """ */
            char *closing = strstr(val_start + 3, "\"\"\"");
            if (closing) {
                val_start = closing + 3;
                /* Skip trailing newline */
                if (*val_start == '\n') val_start++;
            }
        } else if (*val_start == '"') {
            /* Single-line string: find closing " */
            char *closing = strchr(val_start + 1, '"');
            if (closing) {
                val_start = closing + 1;
                if (*val_start == '\n') val_start++;
            }
        }

        /* Write the rest of the file */
        str_append_cstr(&out, val_start);
    } else {
        /* No existing system_prompt_extra — append at end */
        str_append_cstr(&out, data);
        if (data[strlen(data) - 1] != '\n')
            str_append_cstr(&out, "\n");
        str_append_cstr(&out, "\nsystem_prompt_extra = \"\"\"\n");
        str_append_cstr(&out, prompt_text);
        if (prompt_text[strlen(prompt_text) - 1] != '\n')
            str_append_cstr(&out, "\n");
        str_append_cstr(&out, "\"\"\"\n");
    }

    free(data);

    int rc = write_file(profile_path, str_cstr(&out), out.len);
    str_free(&out);

    if (rc == 0)
        fprintf(stderr, "[optimize] wrote optimized prompt to %s\n", profile_path);
    else
        fprintf(stderr, "[optimize] ERROR: failed to write %s\n", profile_path);

    return rc;
}

prompt_candidate_t optimize_run(optimize_config_t *opt,
                                query_bank_t *banks, int n_banks,
                                config_t *cfg,
                                memory_t *memory,
                                store_t *store,
                                const char *nash_dir) {
    prompt_candidate_t best = {0};
    best.held_in_score = -1;
    best.held_out_score = -1;

    fprintf(stderr, "\n╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║      Nash Prompt Optimization (GEPA)    ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════╝\n\n");
    fprintf(stderr, "  Budget: %d rounds\n", opt->max_rounds);
    fprintf(stderr, "  Student model: %s\n",
            opt->student && opt->student->cfg.model_id
                ? opt->student->cfg.model_id : "(unknown)");
    fprintf(stderr, "  Reflection model: %s\n",
            opt->reflection && opt->reflection->cfg.model_id
                ? opt->reflection->cfg.model_id : "(same as student)");
    if (opt->profile_path)
        fprintf(stderr, "  Profile: %s\n", opt->profile_path);
    fprintf(stderr, "\n");

    /* Step 1: Score the current (baseline) prompt */
    const char *current_prompt = cfg->system_prompt_extra;

    fprintf(stderr, "━━━ Round 0: Baseline ━━━\n");
    fprintf(stderr, "  Current prompt: %s\n\n",
            current_prompt && current_prompt[0]
                ? "(existing rules)" : "(empty)");

    /* For the baseline, run regression with current prompt */
    regression_report_t *baseline_report = regression_run(
        banks, n_banks, opt->split_filter,
        opt->student, cfg, memory, store, nash_dir);

    if (!baseline_report) {
        fprintf(stderr, "[optimize] baseline regression run failed\n");
        return best;
    }

    best.prompt_text = current_prompt ? strdup(current_prompt) : NULL;
    best.score = baseline_report->overall_score;
    best.held_in_score = baseline_report->held_in_score;
    best.held_out_score = baseline_report->held_out_score;
    best.total_passed = baseline_report->total_passed;
    best.total_queries = baseline_report->total_queries;
    best.round = 0;

    fprintf(stderr, "\n  Baseline: %.1f%% (%d/%d passed)\n\n",
            best.score * 100, best.total_passed, best.total_queries);

    /* If already perfect, nothing to optimize */
    if (best.score >= 0.999) {
        fprintf(stderr, "  ✓ Perfect score — nothing to optimize!\n\n");
        regression_free_report(baseline_report);
        return best;
    }

    /* Format baseline feedback for first reflection round */
    char *feedback = optimize_format_feedback(baseline_report);
    regression_free_report(baseline_report);

    /* Step 2: Optimization loop */
    char *current = current_prompt ? strdup(current_prompt) : strdup("");

    for (int round = 1; round <= opt->max_rounds; round++) {
        fprintf(stderr, "━━━ Round %d/%d ━━━\n", round, opt->max_rounds);

        /* Reflect: ask LM to propose improved prompt */
        char *candidate_text = optimize_reflect(
            opt->reflection, current, feedback,
            round, opt->max_rounds);

        free(feedback);
        feedback = NULL;

        if (!candidate_text) {
            fprintf(stderr, "  ✗ Reflection failed — skipping round\n\n");
            continue;
        }

        /* Score the candidate — also get the report for feedback */
        fprintf(stderr, "  Scoring candidate...\n");
        regression_report_t *cand_report = NULL;
        prompt_candidate_t candidate = score_prompt(
            candidate_text, round,
            opt, banks, n_banks, cfg, memory, store, nash_dir, &cand_report);
        free(candidate_text);

        fprintf(stderr, "\n  Candidate: %.1f%% (%d/%d passed)",
                candidate.score * 100,
                candidate.total_passed, candidate.total_queries);
        if (candidate.held_in_score >= 0)
            fprintf(stderr, "  [held-in: %.1f%%]", candidate.held_in_score * 100);
        if (candidate.held_out_score >= 0)
            fprintf(stderr, "  [held-out: %.1f%%]", candidate.held_out_score * 100);
        fprintf(stderr, "\n");

        /* Compare against best */
        if (is_better(&candidate, &best)) {
            fprintf(stderr, "  ★ New best! (%.1f%% → %.1f%%)\n\n",
                    best.score * 100, candidate.score * 100);
            optimize_free_candidate(&best);
            best = candidate;

            /* Update current prompt for next reflection round */
            free(current);
            current = best.prompt_text ? strdup(best.prompt_text) : strdup("");

            /* Reuse this report for feedback in the next round */
            if (round < opt->max_rounds && cand_report) {
                feedback = optimize_format_feedback(cand_report);

                /* If perfect, stop early */
                if (best.score >= 0.999) {
                    fprintf(stderr, "  ✓ Perfect score — stopping early!\n\n");
                    free(feedback);
                    feedback = NULL;
                    regression_free_report(cand_report);
                    break;
                }
            }
        } else {
            fprintf(stderr, "  → Not better than current best (%.1f%%)\n\n",
                    best.score * 100);
            optimize_free_candidate(&candidate);

            /* Use the candidate's report for feedback anyway —
             * shows the reflection LM what the CURRENT best still fails on */
            if (round < opt->max_rounds && cand_report) {
                feedback = optimize_format_feedback(cand_report);
            }
        }

        if (cand_report)
            regression_free_report(cand_report);
    }

    free(current);
    free(feedback);

    /* Print final results */
    fprintf(stderr, "\n╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║       Optimization Results              ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════╝\n\n");
    fprintf(stderr, "  Best score: %.1f%% (%d/%d passed) — from round %d\n",
            best.score * 100, best.total_passed, best.total_queries, best.round);
    if (best.held_in_score >= 0)
        fprintf(stderr, "  Held-in:  %.1f%%\n", best.held_in_score * 100);
    if (best.held_out_score >= 0)
        fprintf(stderr, "  Held-out: %.1f%%\n", best.held_out_score * 100);

    fprintf(stderr, "\n  Winning prompt:\n");
    fprintf(stderr, "  ──────────────────────────────────────\n");
    if (best.prompt_text && best.prompt_text[0]) {
        /* Print with indentation */
        const char *line = best.prompt_text;
        while (line && *line) {
            const char *nl = strchr(line, '\n');
            if (nl) {
                fprintf(stderr, "  %.*s\n", (int)(nl - line), line);
                line = nl + 1;
            } else {
                fprintf(stderr, "  %s\n", line);
                break;
            }
        }
    } else {
        fprintf(stderr, "  (empty)\n");
    }
    fprintf(stderr, "  ──────────────────────────────────────\n\n");

    /* Write to profile if requested */
    if (opt->profile_path && best.prompt_text && best.round > 0) {
        write_prompt_to_profile(opt->profile_path, best.prompt_text);
    } else if (best.round == 0) {
        fprintf(stderr, "  No improvement found — keeping original prompt.\n\n");
    }

    return best;
}

/* ── Utility functions ──────────────────────────────── */

int optimize_parse_budget(const char *budget_str) {
    if (!budget_str) return -1;

    if (strcmp(budget_str, "light") == 0)  return OPTIMIZE_LIGHT;
    if (strcmp(budget_str, "medium") == 0) return OPTIMIZE_MEDIUM;
    if (strcmp(budget_str, "heavy") == 0)  return OPTIMIZE_HEAVY;

    /* Try parsing as integer */
    int n = atoi(budget_str);
    if (n > 0 && n <= 50) return n;

    return -1;
}

void optimize_free_candidate(prompt_candidate_t *c) {
    if (!c) return;
    free(c->prompt_text);
    c->prompt_text = NULL;
    c->score = 0;
}
