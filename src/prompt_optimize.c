/*
 * Self-Harness prompt optimization for Nash.
 *
 * Implements the Self-Harness iterative loop [arXiv:2606.09498]:
 *   Stage 1: Weakness Mining — cluster failures by signature
 *   Stage 2: Harness Proposal — K diverse, minimal candidate edits
 *   Stage 3: Proposal Validation — accept only non-regressive edits
 *
 * The loop merges all accepted edits per round (not just best-of-1),
 * tracks rejected proposals across rounds to avoid re-proposing,
 * and provides execution traces from journal_manifest() as evidence.
 */

#include "prompt_optimize.h"
#include "str.h"
#include "cJSON.h"
#include "nash_limits.h"
#include "nash_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Forward declarations for helpers in react.c ────── */

static char *extract_text_from_response(const char *raw) {
    if (!raw || !raw[0]) return NULL;
    raw = skip_whitespace(raw);
    if (!*raw) return NULL;

    if (raw[0] == '{') {
        cJSON *j = cJSON_Parse(raw);
        if (j) {
            cJSON *c = cJSON_GetObjectItem(j, "content");
            if (c && cJSON_IsString(c) && c->valuestring && c->valuestring[0]) {
                char *result = strdup(c->valuestring);
                cJSON_Delete(j);
                return result;
            }
            cJSON *r = cJSON_GetObjectItem(j, "result");
            if (r && cJSON_IsString(r) && r->valuestring && r->valuestring[0]) {
                char *result = strdup(r->valuestring);
                cJSON_Delete(j);
                return result;
            }
            cJSON *t = cJSON_GetObjectItem(j, "thought");
            if (t && cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
                char *result = strdup(t->valuestring);
                cJSON_Delete(j);
                return result;
            }
            cJSON_Delete(j);
        }
    }

    if (strncmp(raw, "```", 3) == 0) {
        const char *start = raw + 3;
        while (*start && *start != '\n') start++;
        if (*start == '\n') start++;
        const char *end = strstr(start, "\n```");
        if (end) return strndup(start, end - start);
    }

    return strdup(raw);
}

/* ════════════════════════════════════════════════════════
 * Stage 1: Weakness Mining [§3.2]
 *
 * "The evaluation system analyzes the trace as evidence for why
 *  the evaluator rejected the run. It identifies the terminal
 *  failure reason exposed by the verifier, the agent-side behavior
 *  connected to that terminal failure, and the causal status of
 *  that behavior within the trace."
 *                               — Self-Harness, §3.2
 * ════════════════════════════════════════════════════════ */

static const char *cause_str(sh_cause_t c) {
    switch (c) {
        case SH_CAUSE_NO_DONE:        return "no_done";
        case SH_CAUSE_WRONG_RESULT:   return "wrong_result";
        case SH_CAUSE_WRONG_TOOL:     return "wrong_tool";
        case SH_CAUSE_TOO_MANY_STEPS: return "too_many_steps";
        case SH_CAUSE_ERRORS:         return "errors";
        case SH_CAUSE_OTHER:          return "other";
    }
    return "other";
}

static const char *mechanism_str(sh_mechanism_t m) {
    switch (m) {
        case SH_MECH_TOOL_CHOICE:    return "wrong_tool_choice";
        case SH_MECH_TASK_ABANDON:   return "task_abandonment";
        case SH_MECH_LOOP:           return "unproductive_loop";
        case SH_MECH_INCOMPLETE:     return "incomplete_result";
        case SH_MECH_ERROR_CASCADE:  return "error_cascade";
        case SH_MECH_OVER_EXPLORE:   return "over_exploration";
        case SH_MECH_OTHER:          return "other";
    }
    return "other";
}

static const char *status_str(sh_status_t s) {
    switch (s) {
        case SH_STATUS_CAUSAL:       return "causal";
        case SH_STATUS_CONTRIBUTING: return "contributing";
        case SH_STATUS_UNKNOWN:      return "unknown";
    }
    return "unknown";
}

/* Classify a single query failure into a failure signature.
 * Implements φ(r_i) = (c_i, q_i, m_i) from §3.2. */
static sh_signature_t classify_failure(const query_result_t *qr) {
    sh_signature_t sig = {
        .cause = SH_CAUSE_OTHER,
        .status = SH_STATUS_UNKNOWN,
        .mechanism = SH_MECH_OTHER,
    };

    int has_status_fail = 0, has_contains_fail = 0;
    int has_tool_fail = 0, has_steps_fail = 0, has_error_fail = 0;

    for (int i = 0; i < qr->n_crit_results; i++) {
        if (qr->crit_results[i].passed) continue;
        const char *desc = qr->crit_results[i].criterion_desc;
        if (!desc) continue;

        if (strstr(desc, "status("))           has_status_fail = 1;
        else if (strstr(desc, "contains("))    has_contains_fail = 1;
        else if (strstr(desc, "not_contains(")) has_contains_fail = 1;
        else if (strstr(desc, "tool_used("))   has_tool_fail = 1;
        else if (strstr(desc, "tool_not_used(")) has_tool_fail = 1;
        else if (strstr(desc, "max_steps("))   has_steps_fail = 1;
        else if (strstr(desc, "no_error"))     has_error_fail = 1;
        else if (strstr(desc, "regex("))       has_contains_fail = 1;
    }

    if (has_status_fail) {
        sig.cause = SH_CAUSE_NO_DONE;
        sig.status = SH_STATUS_CAUSAL;
        sig.mechanism = SH_MECH_TASK_ABANDON;
    } else if (has_tool_fail) {
        sig.cause = SH_CAUSE_WRONG_TOOL;
        sig.status = SH_STATUS_CAUSAL;
        sig.mechanism = SH_MECH_TOOL_CHOICE;
    } else if (has_steps_fail) {
        sig.cause = SH_CAUSE_TOO_MANY_STEPS;
        sig.status = SH_STATUS_CAUSAL;
        sig.mechanism = qr->steps_used > 15 ? SH_MECH_LOOP : SH_MECH_OVER_EXPLORE;
    } else if (has_error_fail) {
        sig.cause = SH_CAUSE_ERRORS;
        sig.status = SH_STATUS_CONTRIBUTING;
        sig.mechanism = SH_MECH_ERROR_CASCADE;
    } else if (has_contains_fail) {
        sig.cause = SH_CAUSE_WRONG_RESULT;
        sig.status = SH_STATUS_CAUSAL;
        sig.mechanism = SH_MECH_INCOMPLETE;
    }

    return sig;
}

static int sig_equals(const sh_signature_t *a, const sh_signature_t *b) {
    return a->cause == b->cause && a->status == b->status && a->mechanism == b->mechanism;
}

static char *build_crit_detail(const query_result_t *qr) {
    str_t s = str_new(256);
    for (int i = 0; i < qr->n_crit_results; i++) {
        if (qr->crit_results[i].passed) continue;
        str_appendf(&s, "  ✗ %s: %s\n",
                    qr->crit_results[i].criterion_desc,
                    qr->crit_results[i].detail ? qr->crit_results[i].detail : "failed");
    }
    return str_steal(&s);
}

static char *truncate_trace(const char *trace, int max_chars) {
    if (!trace) return strdup("(no trace available)");
    int len = (int)strlen(trace);
    if (len <= max_chars) return strdup(trace);
    char *t = malloc(max_chars + 20);
    memcpy(t, trace, max_chars);
    strcpy(t + max_chars, "\n...[truncated]");
    return t;
}

sh_evidence_t *optimize_build_evidence_bundle(const regression_report_t *report) {
    if (!report) return NULL;

    sh_evidence_t *bundle = calloc(1, sizeof(sh_evidence_t));
    str_t pass_summary = str_new(512);

    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        for (int j = 0; j < br->n_results; j++) {
            if (br->results[j].passed) {
                bundle->total_passes++;
                str_appendf(&pass_summary, "  ✓ %s (%.0f%%, %d steps)\n",
                            br->results[j].query_id,
                            br->results[j].score * 100,
                            br->results[j].steps_used);
            } else {
                bundle->total_failures++;
            }
        }
    }
    bundle->passing_summary = str_steal(&pass_summary);

    if (bundle->total_failures == 0) return bundle;

    /* Cluster failures by exact signature agreement (§3.2) */
    int clusters_cap = 8;
    bundle->clusters = calloc(clusters_cap, sizeof(sh_cluster_t));

    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        for (int j = 0; j < br->n_results; j++) {
            query_result_t *qr = &br->results[j];
            if (qr->passed) continue;

            sh_signature_t sig = classify_failure(qr);

            /* Find or create cluster */
            int ci = -1;
            for (int k = 0; k < bundle->n_clusters; k++) {
                if (sig_equals(&bundle->clusters[k].sig, &sig)) { ci = k; break; }
            }

            if (ci < 0) {
                if (bundle->n_clusters >= clusters_cap) {
                    int new_cap = clusters_cap * 2;
                    if (safe_realloc((void **)&bundle->clusters,
                                     (size_t)new_cap * sizeof(sh_cluster_t)))
                        continue;
                    clusters_cap = new_cap;
                }
                ci = bundle->n_clusters++;
                sh_cluster_t *fc = &bundle->clusters[ci];
                memset(fc, 0, sizeof(*fc));
                fc->sig = sig;
                fc->query_ids = calloc(16, sizeof(char *));
                fc->trace_excerpts = calloc(16, sizeof(char *));
                fc->crit_details = calloc(16, sizeof(char *));
            }

            sh_cluster_t *fc = &bundle->clusters[ci];
            int idx = fc->n_entries;
            if (idx >= 16 && (idx & (idx - 1)) == 0) {
                size_t nc = (size_t)idx * 2;
                if (safe_realloc((void **)&fc->query_ids, nc * sizeof(char *)) ||
                    safe_realloc((void **)&fc->trace_excerpts, nc * sizeof(char *)) ||
                    safe_realloc((void **)&fc->crit_details, nc * sizeof(char *)))
                    continue;
            }
            fc->query_ids[idx] = strdup(qr->query_id);
            fc->trace_excerpts[idx] = truncate_trace(qr->trace_summary, 600);
            fc->crit_details[idx] = build_crit_detail(qr);
            fc->n_entries = idx + 1;
            fc->count = idx + 1;
        }
    }

    /* Sort clusters by support count descending (§3.2) */
    for (int i = 0; i < bundle->n_clusters - 1; i++)
        for (int j = i + 1; j < bundle->n_clusters; j++)
            if (bundle->clusters[j].count > bundle->clusters[i].count) {
                sh_cluster_t tmp = bundle->clusters[i];
                bundle->clusters[i] = bundle->clusters[j];
                bundle->clusters[j] = tmp;
            }

    return bundle;
}

void optimize_free_evidence_bundle(sh_evidence_t *b) {
    if (!b) return;
    for (int i = 0; i < b->n_clusters; i++) {
        sh_cluster_t *fc = &b->clusters[i];
        for (int j = 0; j < fc->n_entries; j++) {
            free(fc->query_ids[j]);
            free(fc->trace_excerpts[j]);
            free(fc->crit_details[j]);
        }
        free(fc->query_ids);
        free(fc->trace_excerpts);
        free(fc->crit_details);
    }
    free(b->clusters);
    free(b->passing_summary);
    free(b);
}

/* ════════════════════════════════════════════════════════
 * Evidence Formatting [§3.3]
 * ════════════════════════════════════════════════════════ */

char *optimize_format_evidence(const sh_evidence_t *bundle,
                               const rejected_proposal_t *rejected,
                               int n_rejected) {
    if (!bundle) return NULL;
    str_t fb = str_new(8192);

    str_appendf(&fb, "EVALUATION RESULTS: %d passed, %d failed\n\n",
                bundle->total_passes, bundle->total_failures);

    if (bundle->passing_summary && bundle->passing_summary[0]) {
        str_append_cstr(&fb, "PASSING BEHAVIORS (preserve these):\n");
        str_append_cstr(&fb, bundle->passing_summary);
        str_append_cstr(&fb, "\n");
    }

    if (bundle->n_clusters > 0) {
        str_append_cstr(&fb, "FAILURE PATTERNS (ordered by frequency):\n\n");
        for (int i = 0; i < bundle->n_clusters; i++) {
            sh_cluster_t *fc = &bundle->clusters[i];
            str_appendf(&fb, "Pattern %d: %s/%s/%s (%d failure%s)\n",
                i + 1, cause_str(fc->sig.cause),
                status_str(fc->sig.status),
                mechanism_str(fc->sig.mechanism),
                fc->count, fc->count > 1 ? "s" : "");

            str_append_cstr(&fb, "  Affected queries: ");
            for (int j = 0; j < fc->n_entries; j++) {
                if (j > 0) str_append_cstr(&fb, ", ");
                str_append_cstr(&fb, fc->query_ids[j]);
            }
            str_append_cstr(&fb, "\n");

            for (int j = 0; j < fc->n_entries && j < 3; j++) {
                str_appendf(&fb, "  [%s] Criterion failures:\n%s",
                            fc->query_ids[j], fc->crit_details[j]);
                if (fc->trace_excerpts[j] &&
                    strcmp(fc->trace_excerpts[j], "(no trace available)") != 0)
                    str_appendf(&fb, "  [%s] Execution trace:\n%s\n",
                                fc->query_ids[j], fc->trace_excerpts[j]);
            }
            str_append_cstr(&fb, "\n");
        }
    }

    if (rejected && n_rejected > 0) {
        str_append_cstr(&fb, "PREVIOUSLY REJECTED PROPOSALS (do not re-propose similar edits):\n");
        for (int i = 0; i < n_rejected; i++) {
            str_appendf(&fb, "  Round %d: d_in=%+.1f%%, d_ho=%+.1f%%\n",
                        rejected[i].round,
                        rejected[i].delta_in * 100,
                        rejected[i].delta_out * 100);
            /* SkillOpt: feed rejected text excerpt + audit as negative feedback */
            if (rejected[i].prompt_text && rejected[i].prompt_text[0]) {
                int plen = (int)strlen(rejected[i].prompt_text);
                int show = plen > 200 ? (int)utf8_clamp(rejected[i].prompt_text, 200) : plen;
                str_appendf(&fb, "    Rejected text: %.*s%s\n",
                            show, rejected[i].prompt_text,
                            plen > 200 ? "..." : "");
            }
            if (rejected[i].audit && rejected[i].audit[0])
                str_appendf(&fb, "    Reason: %s\n", rejected[i].audit);
            else
                str_appendf(&fb, "    Reason: regression on validation set\n");
        }
        str_append_cstr(&fb, "\n");
    }

    str_append_cstr(&fb,
        "EDITABLE SURFACES:\n"
        "  1. system_prompt_extra — model-specific rules appended to system prompt\n"
        "  2. tool descriptions — per-tool description overrides in function schema\n"
        "Use === SYSTEM PROMPT === and === TOOL DESCRIPTIONS === section headers.\n"
        "Include EXPECT_FIX: and AT_RISK: predictions for decision manifest.\n");
    return str_steal(&fb);
}

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

    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        for (int j = 0; j < br->n_results; j++) {
            query_result_t *qr = &br->results[j];
            if (qr->passed) {
                str_appendf(&fb, "PASS: %s (%.0f%%, %d steps)\n",
                            qr->query_id, qr->score * 100, qr->steps_used);
                continue;
            }
            str_appendf(&fb, "\nFAIL: %s (score: %.0f%%, %d steps)\n",
                        qr->query_id, qr->score * 100, qr->steps_used);
            for (int k = 0; k < qr->n_crit_results; k++) {
                criterion_result_t *cr = &qr->crit_results[k];
                if (!cr->passed)
                    str_appendf(&fb, "  x %s: %s\n", cr->criterion_desc,
                                cr->detail ? cr->detail : "failed");
            }
            if (qr->result_text) {
                int rlen = (int)strlen(qr->result_text);
                int show = rlen > 300 ? (int)utf8_clamp(qr->result_text, 300) : rlen;
                str_appendf(&fb, "  Result (%d chars): %.*s%s\n",
                            rlen, show, qr->result_text, rlen > 300 ? "..." : "");
            }
        }
    }
    return str_steal(&fb);
}

/* ════════════════════════════════════════════════════════
 * Stage 2: Harness Proposal [§3.3]
 * ════════════════════════════════════════════════════════ */

/* Implementation-blind critic prompt [arXiv:2601.04620, AgentDevel §4.1]:
 *
 * The critic sees ONLY execution traces, rubric, and scores — never the
 * agent's system prompt or internal configuration.  AgentDevel's ablation
 * (Table 3) shows that letting the critic see the blueprint doubles the
 * regression rate (6.7% vs 3.1%) because it over-fits to implementation
 * details rather than diagnosing surface-level symptoms. */
static const char *BLIND_CRITIC_SYSTEM_PROMPT =
    "You are an implementation-blind failure analyst for an LLM coding agent.\n\n"
    "IMPORTANT: You do NOT have access to the agent's system prompt, internal "
    "rules, or configuration.  You can ONLY see execution traces and results.\n\n"
    "YOUR TASK: Analyze the failure evidence and produce a symptom-level "
    "diagnosis.  For each failure pattern:\n"
    "1. Describe the OBSERVABLE symptom (what went wrong in the trace)\n"
    "2. Identify the triggering conditions (what kind of task/input)\n"
    "3. Classify severity (how many queries affected)\n"
    "4. Suggest what BEHAVIOR change would fix it (not prompt changes)\n\n"
    "OUTPUT FORMAT:\n"
    "=== DIAGNOSIS ===\n"
    "PATTERN 1: <symptom description>\n"
    "  Trigger: <what causes it>\n"
    "  Affected: <N queries>\n"
    "  Fix: <behavioral change needed>\n\n"
    "PATTERN 2: ...\n\n"
    "CONSTRAINTS:\n"
    "- Focus on WHAT the agent did wrong, not WHY its prompt caused it\n"
    "- Be specific about observable behaviors (e.g. 'called file_read 3 times "
    "on the same file' not 'inefficient')\n"
    "- Rank patterns by severity (most affected queries first)\n"
    "- Do NOT propose prompt text or rule changes — only diagnose\n";

static const char *REFLECTION_SYSTEM_PROMPT =
    "You are the Self-Harness proposer for Nash, an autonomous coding agent.\n\n"
    "CONTEXT: Nash uses a ReAct loop with tools (file_read, file_write, file_edit, "
    "shell_exec, grep_search, glob_search, web_fetch, web_search, memory_store, "
    "memory_search, notes, done, plan, user_ask). You are improving the agent's "
    "harness configuration.\n\n"
    "SELF-HARNESS PROTOCOL [arXiv:2606.09498]:\n"
    "You will receive a symptom-level diagnosis from a blind critic (who has NOT "
    "seen the current prompt) along with the current prompt rules.  Your job is "
    "to translate the diagnosis into minimal prompt edits.\n\n"
    "EDITABLE SURFACES:\n"
    "1. System prompt rules (model-specific behavioral rules)\n"
    "2. Tool descriptions (how tools are described in the function-calling schema)\n\n"
    "YOUR TASK — propose a BOUNDED, MINIMAL edit:\n"
    "1. Select ONE primary failure pattern from the diagnosis to address\n"
    "2. Propose ONE new rule OR tool description change targeting that mechanism\n"
    "3. You may also mark ONE existing rule for removal if it causes harm\n"
    "4. Include PREDICTIONS about which queries will be fixed/at-risk\n\n"
    "OUTPUT FORMAT (use these exact section headers):\n"
    "=== SYSTEM PROMPT ===\n"
    "<complete updated rule set>\n"
    "=== TOOL DESCRIPTIONS ===\n"
    "[tool_name] <new description for this tool>\n"
    "(only include tools whose descriptions you want to change)\n"
    "EXPECT_FIX: query_id1, query_id2\n"
    "AT_RISK: query_id3\n\n"
    "CONSTRAINTS:\n"
    "- Each proposal must target a DIFFERENT failure pattern than others in this round\n"
    "- Keep rules concise and actionable — not generic advice\n"
    "- Preserve rules that correspond to passing behaviors\n"
    "- Do NOT rewrite the entire prompt — make minimal targeted changes\n"
    "- Keep system prompt rules under 500 words\n"
    "- Tool descriptions should be 1-3 sentences each\n"
    "- EXPECT_FIX/AT_RISK predictions are REQUIRED — they are verified next round\n";

/* Phase 1: Implementation-blind diagnosis [arXiv:2601.04620, AgentDevel §4.1].
 *
 * The blind critic sees ONLY execution traces and scores — never the agent's
 * system prompt.  This prevents the critic from over-fitting to implementation
 * details and forces symptom-level diagnosis.  The paper's ablation shows
 * non-blind critics double the regression rate (6.7% vs 3.1%). */
static char *blind_diagnose(provider_t *reflection_lm,
                            const char *evidence_text,
                            int proposal_idx, int proposal_width) {
    if (!reflection_lm || !evidence_text) return NULL;

    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system", BLIND_CRITIC_SYSTEM_PROMPT);

    str_t user_msg = str_new(4096);
    str_append_cstr(&user_msg, "FAILURE EVIDENCE:\n");
    str_appendf(&user_msg, "%s\n", evidence_text);

    if (proposal_width > 1) {
        str_appendf(&user_msg,
            "\nFocus on Pattern %d from the failure list above.\n",
            (proposal_idx < 6 ? proposal_idx + 1 : 1));
    }

    str_append_cstr(&user_msg,
        "\nProduce your symptom-level diagnosis now.\n");

    llm_chat_add(chat, "user", str_cstr(&user_msg));
    str_free(&user_msg);

    llm_stats_t stats = {0};
    char *response = provider_complete(reflection_lm, chat, &stats);
    llm_chat_free(chat);

    if (!response) {
        fprintf(stderr, "[blind-critic] diagnosis %d/%d failed — no response\n",
                proposal_idx + 1, proposal_width);
        return NULL;
    }

    fprintf(stderr, "[blind-critic] diagnosis %d/%d: %zu chars\n",
            proposal_idx + 1, proposal_width, strlen(response));
    return response;
}

/* Phase 2: Repair proposer — sees the blind critic's diagnosis AND the current
 * prompt, then proposes minimal edits.  This two-phase split ensures the
 * diagnosis is not contaminated by knowledge of the prompt.
 * [arXiv:2601.04620, AgentDevel §4.1] */
char *optimize_reflect(provider_t *reflection_lm,
                       const char *current_prompt,
                       const char *evidence_text,
                       int round, int max_rounds,
                       int proposal_idx, int proposal_width,
                       int edit_budget,
                       const char *slow_guidance) {
    if (!reflection_lm || !evidence_text) return NULL;

    /* Phase 1: blind diagnosis [arXiv:2601.04620] */
    char *diagnosis = blind_diagnose(reflection_lm, evidence_text,
                                      proposal_idx, proposal_width);

    /* Phase 2: repair proposal (sees prompt + diagnosis) */
    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system", REFLECTION_SYSTEM_PROMPT);

    str_t user_msg = str_new(8192);
    str_appendf(&user_msg, "SELF-HARNESS ROUND %d of %d — Proposal %d of %d\n\n",
                round, max_rounds, proposal_idx + 1, proposal_width);

    /* SkillOpt: bounded textual learning rate [arXiv:2605.23904v2, §3.3] */
    if (edit_budget > 0)
        str_appendf(&user_msg,
            "EDIT BUDGET: You may make AT MOST %d edit operations "
            "(add/delete/replace) this round. Fewer is better.\n\n",
            edit_budget);

    /* SkillOpt: slow/meta update — cross-epoch longitudinal guidance */
    if (slow_guidance && slow_guidance[0])
        str_appendf(&user_msg,
            "LONGITUDINAL GUIDANCE (from previous epoch analysis — "
            "DO NOT delete this, use it to inform your edits):\n%s\n\n",
            slow_guidance);

    /* Blind critic diagnosis (Phase 1 output) */
    if (diagnosis && diagnosis[0]) {
        str_append_cstr(&user_msg, "BLIND CRITIC DIAGNOSIS "
            "(from an analyst who has NOT seen the current prompt):\n");
        str_appendf(&user_msg, "---\n%s\n---\n\n", diagnosis);
    } else {
        /* Fallback: if blind critic failed, pass raw evidence */
        str_appendf(&user_msg, "FAILURE EVIDENCE:\n%s\n\n", evidence_text);
    }
    free(diagnosis);

    str_append_cstr(&user_msg, "CURRENT SYSTEM PROMPT RULES:\n");
    if (current_prompt && current_prompt[0])
        str_appendf(&user_msg, "---\n%s\n---\n\n", current_prompt);
    else
        str_append_cstr(&user_msg, "---\n(empty — no model-specific rules yet)\n---\n\n");

    if (proposal_width > 1) {
        str_appendf(&user_msg,
            "\nDIVERSITY REQUIREMENT: This is proposal %d of %d. "
            "Target Pattern %d from the diagnosis above.\n",
            proposal_idx + 1, proposal_width,
            (proposal_idx < 6 ? proposal_idx + 1 : 1));
    }

    if (round > 1)
        str_appendf(&user_msg,
            "\nRound %d — try a DIFFERENT approach from previously rejected proposals.\n",
            round);

    str_append_cstr(&user_msg,
        "\nPropose your MINIMAL, TARGETED edit. Output ONLY the complete updated prompt text.\n");

    llm_chat_add(chat, "user", str_cstr(&user_msg));
    str_free(&user_msg);

    llm_stats_t stats = {0};
    char *raw_response = provider_complete(reflection_lm, chat, &stats);
    llm_chat_free(chat);

    if (!raw_response) {
        fprintf(stderr, "[self-harness] proposal %d/%d failed — no response\n",
                proposal_idx + 1, proposal_width);
        return NULL;
    }

    char *text = extract_text_from_response(raw_response);
    free(raw_response);
    if (!text) {
        fprintf(stderr, "[self-harness] proposal %d/%d — failed to extract text\n",
                proposal_idx + 1, proposal_width);
        return NULL;
    }

    char *start = (char *)skip_whitespace(text);
    rtrim_whitespace(start);
    char *result = strdup(start);
    free(text);

    fprintf(stderr, "[self-harness] proposal %d/%d: %zu chars (round %d)\n",
            proposal_idx + 1, proposal_width, strlen(result), round);
    return result;
}

/* ════════════════════════════════════════════════════════
 * Stage 3: Proposal Validation [§3.4]
 * ════════════════════════════════════════════════════════ */

static prompt_candidate_t score_prompt(const char *prompt_text,
                                        int round,
                                        optimize_config_t *opt,
                                        query_bank_t *banks, int n_banks,
                                        config_t *cfg,
                                        memory_t *memory,
                                        store_t *store,
                                        const char *nash_dir,
                                        regression_report_t **out_report,
                                        char **tool_names,
                                        char **tool_descs,
                                        int n_tool_descs) {
    prompt_candidate_t cand = {0};
    cand.prompt_text = prompt_text ? strdup(prompt_text) : NULL;
    cand.round = round;
    if (out_report) *out_report = NULL;

    /* Save and swap system_prompt_extra */
    const char *saved_extra = cfg->system_prompt_extra;
    cfg->system_prompt_extra = prompt_text;

    /* Save and swap tool descriptions [Rec #2: AHE tool optimization] */
    char **saved_td_names = cfg->profile_tool_desc_names;
    char **saved_td_values = cfg->profile_tool_desc_values;
    int saved_td_count = cfg->n_profile_tool_descs;
    if (tool_names && n_tool_descs > 0) {
        cfg->profile_tool_desc_names = tool_names;
        cfg->profile_tool_desc_values = tool_descs;
        cfg->n_profile_tool_descs = n_tool_descs;
    }

    regression_report_t *report = regression_run(
        banks, n_banks, opt->split_filter,
        opt->student, cfg, memory, store, nash_dir);

    /* Restore originals */
    cfg->system_prompt_extra = saved_extra;
    cfg->profile_tool_desc_names = saved_td_names;
    cfg->profile_tool_desc_values = saved_td_values;
    cfg->n_profile_tool_descs = saved_td_count;

    if (report) {
        cand.score = report->overall_score;
        cand.held_in_score = report->held_in_score;
        cand.held_out_score = report->held_out_score;
        cand.total_passed = report->total_passed;
        cand.total_queries = report->total_queries;
        if (out_report) *out_report = report;
        else regression_free_report(report);
    }
    return cand;
}

/* Self-Harness acceptance rule [§3.4] with flip-centered gating
 * [arXiv:2601.04620, AgentDevel §4.3]:
 *
 * Original rule: Δ_in >= 0 AND Δ_ho >= 0 AND max(Δ_in, Δ_ho) > 0
 *
 * AgentDevel addition: also reject if the pass-to-fail (P2F) regression
 * rate exceeds a threshold.  Their ablation (Table 3) shows that removing
 * the flip gate causes 5x more regressions (14.8% vs 3.1%) and 4 bad
 * releases vs 0, even though aggregate scores are slightly higher.
 *
 * We use a 5% P2F threshold (slightly more lenient than the paper's ~1%
 * because Nash's query banks are smaller, so a single regression can
 * represent a large percentage). */
#define P2F_RATE_THRESHOLD 0.05  /* 5% -- reject if >5% of queries regress */

static int passes_acceptance_rule(const prompt_candidate_t *candidate,
                                  const prompt_candidate_t *baseline,
                                  int n_regressions, int total_queries,
                                  double *out_d_in, double *out_d_ho,
                                  const char **out_reject_reason) {
    double d_in = 0, d_ho = 0;
    if (candidate->held_in_score >= 0 && baseline->held_in_score >= 0)
        d_in = candidate->held_in_score - baseline->held_in_score;
    if (candidate->held_out_score >= 0 && baseline->held_out_score >= 0)
        d_ho = candidate->held_out_score - baseline->held_out_score;

    if (out_d_in) *out_d_in = d_in;
    if (out_d_ho) *out_d_ho = d_ho;
    if (out_reject_reason) *out_reject_reason = NULL;

    /* Flip-centered gate [arXiv:2601.04620, AgentDevel §4.3]:
     * Reject proposals with excessive pass-to-fail regressions regardless
     * of aggregate score improvement.  P2F regressions indicate the
     * proposal breaks previously-working queries. */
    if (total_queries > 0 && n_regressions > 0) {
        double p2f_rate = (double)n_regressions / (double)total_queries;
        if (p2f_rate > P2F_RATE_THRESHOLD) {
            if (out_reject_reason)
                *out_reject_reason = "P2F regression rate exceeded threshold";
            return 0;
        }
    }

    if (candidate->held_in_score >= 0 && baseline->held_in_score >= 0 &&
        candidate->held_out_score >= 0 && baseline->held_out_score >= 0) {
        double max_d = d_in > d_ho ? d_in : d_ho;
        return (d_in >= -0.001 && d_ho >= -0.001 && max_d > 0.001);
    }
    if (candidate->held_in_score >= 0 && baseline->held_in_score >= 0)
        return d_in > 0.001;
    if (candidate->held_out_score >= 0 && baseline->held_out_score >= 0)
        return d_ho > 0.001;
    return (candidate->score > baseline->score + 0.001);
}

/* ════════════════════════════════════════════════════════
 * Per-query regression tracking [AHE-inspired]
 *
 * Compares two regression reports query-by-query to find:
 *   - Fixes:       pass in candidate, fail in baseline
 *   - Regressions: fail in candidate, pass in baseline
 *
 * Closes regression blindness: the acceptance gate knows aggregate
 * deltas, but per-query flips reveal WHICH queries are at risk.
 * ════════════════════════════════════════════════════════ */

/* Find a query_result by ID in a report. Returns NULL if not found. */
static const query_result_t *find_query_in_report(
        const regression_report_t *report, const char *query_id) {
    if (!report || !query_id) return NULL;
    for (int i = 0; i < report->n_bank_results; i++) {
        bank_result_t *br = &report->bank_results[i];
        for (int j = 0; j < br->n_results; j++) {
            if (br->results[j].query_id &&
                strcmp(br->results[j].query_id, query_id) == 0)
                return &br->results[j];
        }
    }
    return NULL;
}

/* Compare two reports per-query. Returns flips array (caller frees).
 * *n_fixes and *n_regressions are set to counts.
 * Public API: optimize_compare_reports_per_query() */
query_flip_t *optimize_compare_reports_per_query(
        const regression_report_t *baseline,
        const regression_report_t *candidate,
        int *out_n_flips, int *out_n_fixes, int *out_n_regressions) {
    *out_n_flips = 0;
    *out_n_fixes = 0;
    *out_n_regressions = 0;
    if (!baseline || !candidate) return NULL;

    /* Count total queries in candidate for alloc */
    int total = 0;
    for (int i = 0; i < candidate->n_bank_results; i++)
        total += candidate->bank_results[i].n_results;
    if (total == 0) return NULL;

    query_flip_t *flips = calloc((size_t)total, sizeof(query_flip_t));
    int n = 0;

    for (int i = 0; i < candidate->n_bank_results; i++) {
        bank_result_t *br = &candidate->bank_results[i];
        for (int j = 0; j < br->n_results; j++) {
            query_result_t *cand_qr = &br->results[j];
            const query_result_t *base_qr =
                find_query_in_report(baseline, cand_qr->query_id);
            if (!base_qr) continue;

            if (base_qr->passed != cand_qr->passed) {
                flips[n].query_id = cand_qr->query_id;
                flips[n].was_pass = base_qr->passed;
                flips[n].now_pass = cand_qr->passed;
                flips[n].delta_score = cand_qr->score - base_qr->score;
                if (cand_qr->passed && !base_qr->passed)
                    (*out_n_fixes)++;
                else
                    (*out_n_regressions)++;
                n++;
            }
        }
    }
    *out_n_flips = n;
    return flips;
}

/* Format per-query flips for stderr logging */
static void log_query_flips(const query_flip_t *flips, int n_flips,
                            int n_fixes, int n_regressions) {
    if (n_flips == 0) return;
    fprintf(stderr, "  Per-query: %d fix%s, %d regression%s\n",
            n_fixes, n_fixes != 1 ? "es" : "",
            n_regressions, n_regressions != 1 ? "s" : "");
    for (int i = 0; i < n_flips; i++) {
        if (flips[i].now_pass)
            fprintf(stderr, "    FIXED: %s (score %+.0f%%)\n",
                    flips[i].query_id, flips[i].delta_score * 100);
        else
            fprintf(stderr, "    REGRESSED: %s (score %+.0f%%)\n",
                    flips[i].query_id, flips[i].delta_score * 100);
    }
}

/* Append flip info to a str_t for rejection audit */
static void append_flip_audit(str_t *audit, const query_flip_t *flips,
                              int n_flips, int n_fixes, int n_regressions) {
    if (n_flips == 0) return;
    str_appendf(audit, ". Per-query: %d fix%s, %d regression%s",
                n_fixes, n_fixes != 1 ? "es" : "",
                n_regressions, n_regressions != 1 ? "s" : "");
    if (n_regressions > 0) {
        str_append_cstr(audit, " [regressed: ");
        int shown = 0;
        for (int i = 0; i < n_flips && shown < 5; i++) {
            if (!flips[i].now_pass) {
                if (shown > 0) str_append_cstr(audit, ", ");
                str_append_cstr(audit, flips[i].query_id);
                shown++;
            }
        }
        if (n_regressions > 5) str_append_cstr(audit, ", ...");
        str_append_cstr(audit, "]");
    }
}

/* MergeAccepted [§3.4]: pick accepted candidate with best score.
 * For text-blob harness surfaces, we use last-writer-wins with best delta. */
static char *merge_accepted_prompts(prompt_candidate_t *accepted, int n) {
    if (n == 0) return strdup("");
    if (n == 1) return strdup(accepted[0].prompt_text);
    int best = 0;
    for (int i = 1; i < n; i++)
        if (accepted[i].score > accepted[best].score) best = i;
    return strdup(accepted[best].prompt_text);
}

static int write_prompt_to_profile(const char *profile_path,
                                    const char *prompt_text) {
    if (!profile_path || !prompt_text) return -1;
    char *data = slurp_file(profile_path, NULL);
    if (!data) {
        fprintf(stderr, "[self-harness] cannot read profile: %s\n", profile_path);
        return -1;
    }

    str_t out = str_new(strlen(data) + strlen(prompt_text) + 256);
    char *spe = strstr(data, "system_prompt_extra");
    if (spe) {
        str_append(&out, data, spe - data);
        str_append_cstr(&out, "system_prompt_extra = \"\"\"\n");
        str_append_cstr(&out, prompt_text);
        if (prompt_text[strlen(prompt_text) - 1] != '\n')
            str_append_cstr(&out, "\n");
        str_append_cstr(&out, "\"\"\"\n");

        char *val_start = spe + strlen("system_prompt_extra");
        while (*val_start == ' ' || *val_start == '=') val_start++;
        if (strncmp(val_start, "\"\"\"", 3) == 0) {
            char *closing = strstr(val_start + 3, "\"\"\"");
            if (closing) { val_start = closing + 3; if (*val_start == '\n') val_start++; }
        } else if (*val_start == '"') {
            char *closing = strchr(val_start + 1, '"');
            if (closing) { val_start = closing + 1; if (*val_start == '\n') val_start++; }
        }
        str_append_cstr(&out, val_start);
    } else {
        str_append_cstr(&out, data);
        if (data[strlen(data) - 1] != '\n') str_append_cstr(&out, "\n");
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
        fprintf(stderr, "[self-harness] wrote optimized prompt to %s\n", profile_path);
    else
        fprintf(stderr, "[self-harness] ERROR: failed to write %s\n", profile_path);
    return rc;
}

/* ════════════════════════════════════════════════════════
 * SkillOpt slow update [arXiv:2605.23904v2, §3.4]
 *
 * At each epoch boundary, compare the skill at epoch start vs end.
 * Generates longitudinal guidance for the next epoch's proposals.
 * ════════════════════════════════════════════════════════ */

static char *slow_update(provider_t *reflection_lm,
                         const char *prev_skill,
                         const char *curr_skill,
                         const sh_evidence_t *prev_evidence,
                         const sh_evidence_t *curr_evidence) {
    if (!reflection_lm) return NULL;
    if (!prev_skill) prev_skill = "(empty)";
    if (!curr_skill) curr_skill = "(empty)";

    /* If skill didn't change, no guidance needed */
    if (strcmp(prev_skill, curr_skill) == 0) {
        fprintf(stderr, "  [slow-update] skill unchanged — skipping\n");
        return NULL;
    }

    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system",
        "You are the SkillOpt slow-update analyzer [arXiv:2605.23904v2].\n\n"
        "You will receive two versions of an agent's skill document (before/after "
        "one epoch of optimization) plus evaluation summaries.\n\n"
        "YOUR TASK: Write 2-4 sentences of LONGITUDINAL GUIDANCE for the next epoch.\n"
        "Categorize observed changes into:\n"
        "  - Improvements: failure patterns that were resolved\n"
        "  - Regressions: passing behaviors that broke\n"
        "  - Persistent failures: still failing despite edits\n"
        "  - Stable successes: consistently passing\n\n"
        "Focus your guidance on:\n"
        "  1. What kinds of edits worked vs didn't\n"
        "  2. Which persistent failure patterns need different approaches\n"
        "  3. Which rules are load-bearing and must be preserved\n\n"
        "Be specific and actionable. Output ONLY the guidance text.\n");

    str_t user_msg = str_new(4096);
    str_append_cstr(&user_msg, "SKILL AT EPOCH START:\n---\n");
    str_append_cstr(&user_msg, prev_skill);
    str_append_cstr(&user_msg, "\n---\n\nSKILL AT EPOCH END:\n---\n");
    str_append_cstr(&user_msg, curr_skill);
    str_append_cstr(&user_msg, "\n---\n\n");

    if (prev_evidence) {
        str_appendf(&user_msg, "EVAL AT EPOCH START: %d passed, %d failed\n",
                    prev_evidence->total_passes, prev_evidence->total_failures);
        for (int i = 0; i < prev_evidence->n_clusters && i < 4; i++)
            str_appendf(&user_msg, "  Pattern: %s/%s (%d failures)\n",
                        cause_str(prev_evidence->clusters[i].sig.cause),
                        mechanism_str(prev_evidence->clusters[i].sig.mechanism),
                        prev_evidence->clusters[i].count);
    }

    if (curr_evidence) {
        str_appendf(&user_msg, "\nEVAL AT EPOCH END: %d passed, %d failed\n",
                    curr_evidence->total_passes, curr_evidence->total_failures);
        for (int i = 0; i < curr_evidence->n_clusters && i < 4; i++)
            str_appendf(&user_msg, "  Pattern: %s/%s (%d failures)\n",
                        cause_str(curr_evidence->clusters[i].sig.cause),
                        mechanism_str(curr_evidence->clusters[i].sig.mechanism),
                        curr_evidence->clusters[i].count);
    }

    str_append_cstr(&user_msg,
        "\nWrite 2-4 sentences of longitudinal guidance for the next epoch.\n");

    llm_chat_add(chat, "user", str_cstr(&user_msg));
    str_free(&user_msg);

    llm_stats_t stats = {0};
    char *raw = provider_complete(reflection_lm, chat, &stats);
    llm_chat_free(chat);

    if (!raw) {
        fprintf(stderr, "  [slow-update] LLM call failed\n");
        return NULL;
    }

    char *text = extract_text_from_response(raw);
    free(raw);

    if (text) {
        fprintf(stderr, "  [slow-update] generated %zu chars of guidance\n", strlen(text));
    }
    return text;
}

/* ════════════════════════════════════════════════════════
 * Cosine edit budget schedule [arXiv:2605.23904v2, §3.3]
 *
 * L_t = floor + (init - floor) * cos(π * t / (2 * T))
 * Decays from L_0 to L_min over T rounds per epoch.
 * ════════════════════════════════════════════════════════ */

static int compute_edit_budget(int init, int floor_val, int round, int max_rounds) {
    if (init <= 0) return 0;  /* 0 = unlimited */
    if (max_rounds <= 1) return init;
    double progress = (double)round / (double)max_rounds;
    if (progress > 1.0) progress = 1.0;
    double decayed = floor_val + (init - floor_val) * cos(M_PI * progress / 2.0);
    int budget = (int)(decayed + 0.5);
    if (budget < floor_val) budget = floor_val;
    if (budget < 1) budget = 1;
    return budget;
}

/* ════════════════════════════════════════════════════════
 * Algorithm 1: SkillOpt Loop [arXiv:2605.23904v2]
 *
 * Extends Self-Harness [arXiv:2606.09498] with:
 *   - Multi-epoch training with slow/meta update
 *   - Bounded textual learning rate with cosine decay
 *   - Enriched rejected-edit feedback as negative gradients
 *   - Optional minibatch reflection
 * ════════════════════════════════════════════════════════ */

prompt_candidate_t optimize_run(optimize_config_t *opt,
                                query_bank_t *banks, int n_banks,
                                config_t *cfg,
                                memory_t *memory,
                                store_t *store,
                                const char *nash_dir) {
    prompt_candidate_t best = {0};
    best.held_in_score = -1;
    best.held_out_score = -1;

    int K = opt->proposal_width > 0 ? opt->proposal_width : 2;
    int n_epochs = opt->n_epochs > 1 ? opt->n_epochs : 1;
    int edit_init = opt->edit_budget_init > 0 ? opt->edit_budget_init : 4;
    int edit_floor = opt->edit_budget_floor > 0 ? opt->edit_budget_floor : 2;
    /* int mb_size = opt->minibatch_size > 0 ? opt->minibatch_size : 0; */

    fprintf(stderr, "\n╔══════════════════════════════════════════════════╗\n");
    if (n_epochs > 1)
        fprintf(stderr, "║  Nash SkillOpt Optimizer [arXiv:2605.23904v2]   ║\n");
    else
        fprintf(stderr, "║  Nash Self-Harness Optimizer [arXiv:2606.09498]  ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════════════╝\n\n");
    fprintf(stderr, "  Rounds per epoch (T): %d\n", opt->max_rounds);
    fprintf(stderr, "  Epochs (E): %d\n", n_epochs);
    fprintf(stderr, "  Proposal width (K): %d\n", K);
    fprintf(stderr, "  Edit budget: %d→%d (cosine decay)\n", edit_init, edit_floor);
    fprintf(stderr, "  Student model: %s\n",
            opt->student && opt->student->cfg.model_id
                ? opt->student->cfg.model_id : "(unknown)");
    fprintf(stderr, "  Reflection model: %s\n",
            opt->reflection && opt->reflection->cfg.model_id
                ? opt->reflection->cfg.model_id : "(same as student)");
    if (opt->profile_path)
        fprintf(stderr, "  Profile: %s\n", opt->profile_path);
    fprintf(stderr, "\n");

    /* Rejected proposal history (persists across epochs) */
    int rejected_cap = 16;
    rejected_proposal_t *rejected = calloc(rejected_cap, sizeof(rejected_proposal_t));
    int n_rejected = 0;

    /* Round 0: Baseline */
    const char *current_prompt = cfg->system_prompt_extra;
    fprintf(stderr, "━━━ Baseline Evaluation ━━━\n");

    regression_report_t *baseline_report = regression_run(
        banks, n_banks, opt->split_filter,
        opt->student, cfg, memory, store, nash_dir);

    if (!baseline_report) {
        fprintf(stderr, "[self-harness] baseline regression run failed\n");
        free(rejected);
        return best;
    }

    best.prompt_text = current_prompt ? strdup(current_prompt) : NULL;
    best.score = baseline_report->overall_score;
    best.held_in_score = baseline_report->held_in_score;
    best.held_out_score = baseline_report->held_out_score;
    best.total_passed = baseline_report->total_passed;
    best.total_queries = baseline_report->total_queries;
    best.round = 0;

    fprintf(stderr, "\n  Baseline: %.1f%% (%d/%d passed)\n",
            best.score * 100, best.total_passed, best.total_queries);
    if (best.held_in_score >= 0)
        fprintf(stderr, "  Held-in:  %.1f%%\n", best.held_in_score * 100);
    if (best.held_out_score >= 0)
        fprintf(stderr, "  Held-out: %.1f%%\n", best.held_out_score * 100);

    if (best.score >= 0.999) {
        fprintf(stderr, "  Perfect score — stopping!\n");
        regression_free_report(baseline_report);
        free(rejected);
        return best;
    }

    sh_evidence_t *evidence = optimize_build_evidence_bundle(baseline_report);

    /* Keep baseline report alive for per-query regression tracking.
     * round_baseline_report tracks the current baseline for flip detection. */
    regression_report_t *round_baseline_report = baseline_report;
    baseline_report = NULL;  /* ownership transferred */

    char *current = current_prompt ? strdup(current_prompt) : strdup("");
    prompt_candidate_t round_baseline = best;
    round_baseline.prompt_text = strdup(current);

    /* SkillOpt: cross-epoch longitudinal guidance */
    char *slow_guidance = NULL;
    int stop_early = 0;

    /* Signal-based early stopping [arXiv:2601.04620, AgentDevel §4.2]:
     * Stop when repeated RC rejections indicate the optimizer has converged
     * or the proposal distribution has exhausted useful edits.  The paper
     * recommends stopping on 3+ consecutive rejections. */
    int consecutive_rejects = 0;

    /* ══ Epoch loop [arXiv:2605.23904v2, §3.2] ══ */
    for (int epoch = 0; epoch < n_epochs && !stop_early; epoch++) {
        char *epoch_start_skill = strdup(current);
        sh_evidence_t *epoch_start_evidence = NULL;

        if (n_epochs > 1)
            fprintf(stderr, "\n╔═ Epoch %d/%d ═══════════════════════════════════╗\n",
                    epoch + 1, n_epochs);

        /* Capture epoch-start evidence for slow update */
        if (evidence) {
            /* Shallow snapshot: just copy the summary counts */
            epoch_start_evidence = calloc(1, sizeof(sh_evidence_t));
            epoch_start_evidence->total_passes = evidence->total_passes;
            epoch_start_evidence->total_failures = evidence->total_failures;
            epoch_start_evidence->n_clusters = evidence->n_clusters;
            epoch_start_evidence->clusters = calloc(
                evidence->n_clusters > 0 ? evidence->n_clusters : 1,
                sizeof(sh_cluster_t));
            for (int i = 0; i < evidence->n_clusters; i++) {
                epoch_start_evidence->clusters[i].sig = evidence->clusters[i].sig;
                epoch_start_evidence->clusters[i].count = evidence->clusters[i].count;
                /* Don't deep-copy strings — only used for summary in slow_update */
            }
        }

        /* ── Inner round loop (Self-Harness iteration) ── */
        for (int round = 1; round <= opt->max_rounds; round++) {
            int global_round = epoch * opt->max_rounds + round;
            if (n_epochs > 1)
                fprintf(stderr, "\n━━━ Epoch %d, Round %d/%d (global %d) ━━━\n",
                        epoch + 1, round, opt->max_rounds, global_round);
            else
                fprintf(stderr, "\n━━━ Self-Harness Round %d/%d ━━━\n",
                        round, opt->max_rounds);

            if (!evidence || evidence->total_failures == 0) {
                fprintf(stderr, "  No failures — stopping early\n");
                stop_early = 1;
                break;
            }
            fprintf(stderr, "  Evidence: %d patterns, %d failures\n",
                    evidence->n_clusters, evidence->total_failures);

            /* SkillOpt: compute cosine-decayed edit budget */
            int edit_budget = compute_edit_budget(edit_init, edit_floor,
                                                  round, opt->max_rounds);
            if (edit_budget > 0)
                fprintf(stderr, "  Edit budget: %d\n", edit_budget);

            char *evidence_text = optimize_format_evidence(evidence,
                                                           rejected, n_rejected);

            /* Stage 2: Parallel Propose */
            fprintf(stderr, "  Generating %d proposals...\n", K);
            char **cand_texts = calloc(K, sizeof(char *));
            int n_valid = 0;
            for (int k = 0; k < K; k++) {
                cand_texts[k] = optimize_reflect(opt->reflection, current,
                    evidence_text, round, opt->max_rounds, k, K,
                    edit_budget, slow_guidance);
                if (cand_texts[k]) n_valid++;
            }
            free(evidence_text);

            if (n_valid == 0) {
                fprintf(stderr, "  All proposals failed — skipping round\n");
                free(cand_texts);
                continue;
            }

            /* Stage 3: Validate each candidate */
            prompt_candidate_t *accepted_list = calloc(K, sizeof(prompt_candidate_t));
            int n_accepted = 0;
            regression_report_t *best_report = NULL;

            /* Rec #7: Track manifests from this round for verification */
            manifest_entry_t *round_manifests = calloc(K, sizeof(manifest_entry_t));
            /* Rec #2: Track per-candidate tool description overrides */
            char ***cand_td_names = calloc(K, sizeof(char **));
            char ***cand_td_descs = calloc(K, sizeof(char **));
            int *cand_td_counts = calloc(K, sizeof(int));

            for (int k = 0; k < K; k++) {
                if (!cand_texts[k]) continue;
                fprintf(stderr, "\n  ── Candidate %d/%d ──\n", k + 1, K);

                /* Rec #2: Parse structured proposal (system prompt + tool descs) */
                char **td_names = NULL, **td_descs = NULL;
                int n_td = 0;
                char *prompt_only = optimize_parse_proposal(cand_texts[k],
                    &td_names, &td_descs, &n_td);
                if (!prompt_only) prompt_only = strdup(cand_texts[k]);

                /* Rec #7: Parse decision manifest predictions */
                round_manifests[k] = optimize_parse_manifest(cand_texts[k]);
                if (round_manifests[k].n_expect_fix > 0 ||
                    round_manifests[k].n_at_risk > 0) {
                    fprintf(stderr, "  Manifest: expect_fix=%d, at_risk=%d\n",
                            round_manifests[k].n_expect_fix,
                            round_manifests[k].n_at_risk);
                }

                /* Save parsed tool descs for potential later use */
                cand_td_names[k] = td_names;
                cand_td_descs[k] = td_descs;
                cand_td_counts[k] = n_td;

                regression_report_t *rpt = NULL;
                prompt_candidate_t cand = score_prompt(prompt_only, global_round,
                    opt, banks, n_banks, cfg, memory, store, nash_dir, &rpt,
                    td_names, td_descs, n_td);

                fprintf(stderr, "  Score: %.1f%% (%d/%d)",
                        cand.score * 100, cand.total_passed, cand.total_queries);
                if (cand.held_in_score >= 0)
                    fprintf(stderr, " [in: %.1f%%]", cand.held_in_score * 100);
                if (cand.held_out_score >= 0)
                    fprintf(stderr, " [out: %.1f%%]", cand.held_out_score * 100);

                /* Per-query regression tracking [AHE-inspired] */
                int n_flips = 0, n_fixes = 0, n_regr = 0;
                query_flip_t *flips = optimize_compare_reports_per_query(
                    round_baseline_report, rpt, &n_flips, &n_fixes, &n_regr);

                /* Rec #7: Verify manifest predictions against actual flips */
                if (round_manifests[k].n_expect_fix > 0 ||
                    round_manifests[k].n_at_risk > 0) {
                    optimize_verify_manifest(&round_manifests[k],
                        flips, n_flips, n_fixes, n_regr);
                    char *mf = optimize_format_manifest_feedback(&round_manifests[k]);
                    if (mf) {
                        fprintf(stderr, "  %s", mf);
                        free(mf);
                    }
                }

                double d_in = 0, d_ho = 0;
                const char *reject_reason = NULL;
                int accept = passes_acceptance_rule(&cand, &round_baseline,
                                                    n_regr, cand.total_queries,
                                                    &d_in, &d_ho,
                                                    &reject_reason);

                if (accept) {
                    fprintf(stderr, " ACCEPTED (d_in=%+.1f%%, d_ho=%+.1f%%",
                            d_in * 100, d_ho * 100);
                    if (n_fixes > 0 || n_regr > 0)
                        fprintf(stderr, ", F2P=%d, P2F=%d", n_fixes, n_regr);
                    fprintf(stderr, ")\n");
                    log_query_flips(flips, n_flips, n_fixes, n_regr);
                    accepted_list[n_accepted] = cand;
                    accepted_list[n_accepted].prompt_text = strdup(cand.prompt_text);
                    n_accepted++;
                    if (!best_report || cand.score > best.score) {
                        if (best_report) regression_free_report(best_report);
                        best_report = rpt;
                        rpt = NULL;
                    }
                } else {
                    fprintf(stderr, " REJECTED (d_in=%+.1f%%, d_ho=%+.1f%%",
                            d_in * 100, d_ho * 100);
                    if (n_fixes > 0 || n_regr > 0)
                        fprintf(stderr, ", F2P=%d, P2F=%d", n_fixes, n_regr);
                    if (reject_reason)
                        fprintf(stderr, " [%s]", reject_reason);
                    fprintf(stderr, ")\n");
                    log_query_flips(flips, n_flips, n_fixes, n_regr);
                    if (n_rejected >= rejected_cap) {
                        int new_cap = rejected_cap * 2;
                        if (safe_realloc((void **)&rejected,
                                         (size_t)new_cap * sizeof(rejected_proposal_t)))
                            goto skip_reject;
                        rejected_cap = new_cap;
                    }
                    rejected[n_rejected].prompt_text = strdup(cand_texts[k]);
                    /* SkillOpt: generate brief rejection audit with per-query detail */
                    {
                        str_t audit = str_new(256);
                        if (reject_reason)
                            str_appendf(&audit, "%s (P2F=%d/%d=%.1f%%)",
                                        reject_reason, n_regr,
                                        cand.total_queries,
                                        cand.total_queries > 0
                                            ? 100.0 * n_regr / cand.total_queries
                                            : 0.0);
                        else if (d_in < -0.001 && d_ho < -0.001)
                            str_appendf(&audit, "Regressed on both held-in (%.1f%%) "
                                        "and held-out (%.1f%%)", d_in * 100, d_ho * 100);
                        else if (d_in < -0.001)
                            str_appendf(&audit, "Regressed on held-in (%.1f%%) "
                                        "despite held-out improvement", d_in * 100);
                        else if (d_ho < -0.001)
                            str_appendf(&audit, "Regressed on held-out (%.1f%%) "
                                        "despite held-in improvement", d_ho * 100);
                        else
                            str_appendf(&audit, "No net improvement (d_in=%.1f%%, "
                                        "d_ho=%.1f%%)", d_in * 100, d_ho * 100);
                        append_flip_audit(&audit, flips, n_flips, n_fixes, n_regr);
                        rejected[n_rejected].audit = str_steal(&audit);
                    }
                    rejected[n_rejected].round = global_round;
                    rejected[n_rejected].delta_in = d_in;
                    rejected[n_rejected].delta_out = d_ho;
                    n_rejected++;
skip_reject: ;
                }
                free(flips);
                free(prompt_only);
                if (rpt) regression_free_report(rpt);
                optimize_free_candidate(&cand);
            }
            /* Cleanup per-candidate tool desc allocations */
            for (int k = 0; k < K; k++) {
                for (int t = 0; t < cand_td_counts[k]; t++) {
                    free(cand_td_names[k][t]);
                    free(cand_td_descs[k][t]);
                }
                free(cand_td_names[k]);
                free(cand_td_descs[k]);
                optimize_free_manifest(&round_manifests[k]);
            }
            free(cand_td_names);
            free(cand_td_descs);
            free(cand_td_counts);
            free(round_manifests);

            /* Merge accepted (§3.4) */
            if (n_accepted > 0) {
                fprintf(stderr, "\n  Merging %d accepted proposal%s\n",
                        n_accepted, n_accepted > 1 ? "s" : "");
                char *merged = merge_accepted_prompts(accepted_list, n_accepted);
                free(current);
                current = merged;

                for (int a = 0; a < n_accepted; a++) {
                    if (accepted_list[a].score > best.score) {
                        optimize_free_candidate(&best);
                        best.prompt_text = strdup(accepted_list[a].prompt_text);
                        best.score = accepted_list[a].score;
                        best.held_in_score = accepted_list[a].held_in_score;
                        best.held_out_score = accepted_list[a].held_out_score;
                        best.total_passed = accepted_list[a].total_passed;
                        best.total_queries = accepted_list[a].total_queries;
                        best.round = global_round;
                    }
                }

                optimize_free_candidate(&round_baseline);
                round_baseline.prompt_text = strdup(current);
                round_baseline.score = best.score;
                round_baseline.held_in_score = best.held_in_score;
                round_baseline.held_out_score = best.held_out_score;
                round_baseline.total_passed = best.total_passed;
                round_baseline.total_queries = best.total_queries;

                /* Update per-query baseline for next round's flip detection */
                if (best_report) {
                    regression_free_report(round_baseline_report);
                    round_baseline_report = best_report;
                    best_report = NULL;  /* ownership transferred */
                }
            } else {
                fprintf(stderr, "\n  No proposals accepted — h_{t+1} = h_t\n");
            }

            /* Signal-based stopping: track consecutive rejected rounds
             * [arXiv:2601.04620, AgentDevel §4.2] */
            if (n_accepted > 0)
                consecutive_rejects = 0;
            else
                consecutive_rejects++;

            /* Rebuild evidence for next round */
            optimize_free_evidence_bundle(evidence);
            evidence = round_baseline_report
                ? optimize_build_evidence_bundle(round_baseline_report) : NULL;
            if (best_report) regression_free_report(best_report);

            for (int a = 0; a < n_accepted; a++)
                optimize_free_candidate(&accepted_list[a]);
            free(accepted_list);
            free_string_array(cand_texts, K);

            if (best.score >= 0.999) {
                fprintf(stderr, "  Perfect score — stopping early!\n");
                stop_early = 1;
                break;
            }

            /* Signal-based early stopping [arXiv:2601.04620, AgentDevel §4.2]:
             * 3 consecutive rounds with all proposals rejected indicates
             * the optimizer has converged or exhausted useful edits. */
            if (consecutive_rejects >= 3) {
                fprintf(stderr, "  %d consecutive rejected rounds "
                        "-- stopping (optimizer converged)\n",
                        consecutive_rejects);
                stop_early = 1;
                break;
            }
        } /* end inner round loop */

        /* ── Epoch boundary: slow update [arXiv:2605.23904v2, §3.4] ── */
        if (n_epochs > 1 && !stop_early && epoch < n_epochs - 1) {
            fprintf(stderr, "\n═══ Epoch %d/%d boundary — slow update ═══\n",
                    epoch + 1, n_epochs);

            free(slow_guidance);
            slow_guidance = slow_update(opt->reflection,
                                        epoch_start_skill, current,
                                        epoch_start_evidence, evidence);

            if (slow_guidance)
                fprintf(stderr, "  Guidance: %.120s%s\n",
                        slow_guidance, strlen(slow_guidance) > 120 ? "..." : "");

            /* Re-eval at epoch boundary to get fresh evidence for next epoch */
            if (!evidence) {
                regression_report_t *epoch_end_report = regression_run(
                    banks, n_banks, opt->split_filter,
                    opt->student, cfg, memory, store, nash_dir);
                if (epoch_end_report) {
                    evidence = optimize_build_evidence_bundle(epoch_end_report);
                    regression_free_report(epoch_end_report);
                }
            }
        }

        /* Cleanup epoch-start snapshot */
        free(epoch_start_skill);
        if (epoch_start_evidence) {
            free(epoch_start_evidence->clusters);
            free(epoch_start_evidence);
        }
    } /* end epoch loop */

    /* Cleanup */
    free(current);
    free(slow_guidance);
    optimize_free_candidate(&round_baseline);
    regression_free_report(round_baseline_report);
    for (int i = 0; i < n_rejected; i++) {
        free(rejected[i].prompt_text);
        free(rejected[i].audit);
    }
    free(rejected);

    /* Print final results */
    fprintf(stderr, "\n╔══════════════════════════════════════════════════╗\n");
    if (n_epochs > 1)
        fprintf(stderr, "║       SkillOpt Optimization Results              ║\n");
    else
        fprintf(stderr, "║       Self-Harness Optimization Results          ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════════════╝\n\n");
    fprintf(stderr, "  Best score: %.1f%% (%d/%d) from round %d\n",
            best.score * 100, best.total_passed, best.total_queries, best.round);
    if (best.held_in_score >= 0)
        fprintf(stderr, "  Held-in:  %.1f%%\n", best.held_in_score * 100);
    if (best.held_out_score >= 0)
        fprintf(stderr, "  Held-out: %.1f%%\n", best.held_out_score * 100);

    fprintf(stderr, "\n  Winning prompt:\n  ───────────────\n");
    if (best.prompt_text && best.prompt_text[0]) {
        const char *line = best.prompt_text;
        while (line && *line) {
            const char *nl = strchr(line, '\n');
            if (nl) { fprintf(stderr, "  %.*s\n", (int)(nl - line), line); line = nl + 1; }
            else { fprintf(stderr, "  %s\n", line); break; }
        }
    } else {
        fprintf(stderr, "  (empty)\n");
    }
    fprintf(stderr, "  ───────────────\n\n");

    if (opt->profile_path && best.prompt_text && best.round > 0)
        write_prompt_to_profile(opt->profile_path, best.prompt_text);
    else if (best.round == 0)
        fprintf(stderr, "  No improvement found — keeping original prompt.\n\n");

    /* Rec #1: Generate lessons from failure patterns [AHE memory lever]
     * Run after optimization loop so lessons capture the FINAL failure state —
     * patterns that persisted despite prompt improvements are the ones worth
     * encoding as long-term memory. */
    if (opt->generate_lessons && evidence && evidence->total_failures > 0) {
        fprintf(stderr, "\n━━━ Generating Lessons from Failure Patterns ━━━\n");
        const char *model_name = (opt->student && opt->student->cfg.model_id)
            ? opt->student->cfg.model_id : NULL;
        int n_lessons = optimize_generate_lessons(opt->reflection,
                                                   evidence, memory, model_name);
        if (n_lessons > 0)
            fprintf(stderr, "  %d lesson%s stored in memory\n",
                    n_lessons, n_lessons > 1 ? "s" : "");
        else
            fprintf(stderr, "  No lessons generated\n");
    }

    optimize_free_evidence_bundle(evidence);

    return best;
}

int optimize_parse_budget(const char *budget_str) {
    if (!budget_str) return -1;
    if (strcmp(budget_str, "light") == 0)  return OPTIMIZE_LIGHT;
    if (strcmp(budget_str, "medium") == 0) return OPTIMIZE_MEDIUM;
    if (strcmp(budget_str, "heavy") == 0)  return OPTIMIZE_HEAVY;
    int n = atoi(budget_str);
    if (n > 0 && n <= 50) return n;
    return -1;
}

void optimize_free_candidate(prompt_candidate_t *c) {
    if (!c) return;
    free(c->prompt_text);
    free(c->audit);
    c->prompt_text = NULL;
    c->audit = NULL;
    c->score = 0;
}

/* ════════════════════════════════════════════════════════
 * Rec #1: Memory Lesson Generation [AHE ablation: +5.6pp]
 *
 * AHE's ablation showed long-term memory is the single most impactful
 * harness component (+5.6pp overall, +11.6pp on hard tasks). This
 * function analyzes failure clusters from the evidence bundle and
 * generates actionable lessons that are stored in Nash's memory system.
 *
 * Each lesson encodes a boundary-case behavior pattern that the agent
 * should remember across tasks — analogous to AHE's "12 boundary-case
 * lessons" that drove their biggest gains.
 * ════════════════════════════════════════════════════════ */

static const char *LESSON_GEN_SYSTEM_PROMPT =
    "You are a lesson extraction system for Nash, an autonomous coding agent.\n\n"
    "CONTEXT: Nash uses a ReAct loop with tools (file_read, file_write, file_edit, "
    "shell_exec, grep_search, glob_search, web_fetch, web_search, memory_store, "
    "memory_search, notes, done, plan, user_ask). After running regression tests, "
    "some queries failed. You will analyze the failure patterns and extract "
    "reusable lessons.\n\n"
    "YOUR TASK: Generate 1-3 concise, actionable lessons from the failure patterns.\n"
    "Each lesson should:\n"
    "  1. Identify a specific behavioral anti-pattern that caused failures\n"
    "  2. Provide a concrete corrective rule (what to do instead)\n"
    "  3. Be general enough to apply across similar tasks\n"
    "  4. Be specific enough to be actionable (not generic advice)\n\n"
    "OUTPUT FORMAT: Output each lesson as:\n"
    "LESSON: <key-slug>\n"
    "<lesson text — 2-4 sentences>\n"
    "---\n\n"
    "Example:\n"
    "LESSON: verify-file-edit-applied\n"
    "After using file_edit, always verify the edit was applied by reading the "
    "modified region with file_read. file_edit silently fails when old_text "
    "doesn't match exactly due to whitespace differences. A quick verification "
    "read costs minimal tokens but prevents cascading errors.\n"
    "---\n\n"
    "Focus on the MOST impactful patterns — those affecting the most queries.\n"
    "Do NOT generate lessons for patterns that are model-specific quirks.\n";

int optimize_generate_lessons(provider_t *reflection_lm,
                              const sh_evidence_t *evidence,
                              memory_t *memory,
                              const char *model_name) {
    if (!reflection_lm || !evidence || !memory) return 0;
    if (evidence->total_failures == 0) return 0;

    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system", LESSON_GEN_SYSTEM_PROMPT);

    str_t user_msg = str_new(4096);
    str_appendf(&user_msg, "FAILURE ANALYSIS (from regression testing on %s):\n\n",
                model_name ? model_name : "unknown model");
    str_appendf(&user_msg, "Total: %d passed, %d failed\n\n",
                evidence->total_passes, evidence->total_failures);

    for (int i = 0; i < evidence->n_clusters && i < 5; i++) {
        sh_cluster_t *fc = &evidence->clusters[i];
        str_appendf(&user_msg, "Pattern %d: %s/%s/%s (%d failure%s)\n",
            i + 1, cause_str(fc->sig.cause),
            status_str(fc->sig.status),
            mechanism_str(fc->sig.mechanism),
            fc->count, fc->count > 1 ? "s" : "");

        str_append_cstr(&user_msg, "  Affected: ");
        for (int j = 0; j < fc->n_entries && j < 5; j++) {
            if (j > 0) str_append_cstr(&user_msg, ", ");
            str_append_cstr(&user_msg, fc->query_ids[j]);
        }
        str_append_cstr(&user_msg, "\n");

        for (int j = 0; j < fc->n_entries && j < 2; j++) {
            str_appendf(&user_msg, "  [%s] Criteria:\n%s",
                        fc->query_ids[j], fc->crit_details[j]);
            if (fc->trace_excerpts[j] &&
                strcmp(fc->trace_excerpts[j], "(no trace available)") != 0)
                str_appendf(&user_msg, "  [%s] Trace:\n%s\n",
                            fc->query_ids[j], fc->trace_excerpts[j]);
        }
        str_append_cstr(&user_msg, "\n");
    }

    str_append_cstr(&user_msg,
        "\nExtract 1-3 lessons from these failures. "
        "Output ONLY lessons in the specified format.\n");

    llm_chat_add(chat, "user", str_cstr(&user_msg));
    str_free(&user_msg);

    llm_stats_t stats = {0};
    char *raw = provider_complete(reflection_lm, chat, &stats);
    llm_chat_free(chat);

    if (!raw) {
        fprintf(stderr, "[optimize] lesson generation failed — no response\n");
        return 0;
    }

    char *text = extract_text_from_response(raw);
    free(raw);
    if (!text) return 0;

    /* Parse lessons from output */
    int n_stored = 0;
    const char *p = text;
    while ((p = strstr(p, "LESSON:")) != NULL) {
        p += 7; /* skip "LESSON:" */
        while (*p == ' ') p++;

        /* Extract slug */
        const char *slug_end = p;
        while (*slug_end && *slug_end != '\n') slug_end++;
        if (slug_end == p) { p = slug_end; continue; }

        char slug[128];
        int slug_len = (int)(slug_end - p);
        if (slug_len > 120) slug_len = 120;
        memcpy(slug, p, slug_len);
        slug[slug_len] = '\0';
        /* Sanitize slug */
        for (int i = 0; i < slug_len; i++) {
            if (slug[i] == ' ') slug[i] = '-';
            else slug[i] = (char)tolower((unsigned char)slug[i]);
        }

        p = slug_end;
        if (*p == '\n') p++;

        /* Extract lesson body until "---" */
        const char *body_end = strstr(p, "---");
        if (!body_end) body_end = p + strlen(p);

        int body_len = (int)(body_end - p);
        while (body_len > 0 && (p[body_len - 1] == '\n' || p[body_len - 1] == ' '))
            body_len--;

        if (body_len < 10) { p = body_end; continue; }

        char *value = strndup(p, body_len);

        /* Build key: lesson:opt-<slug> */
        char key[192];
        snprintf(key, sizeof(key), "lesson:opt-%s", slug);

        /* Store in memory (non-pinned, no journal ref) */
        int rc = memory_store(memory, key, value, 0, NULL, NULL, 0, NULL, 0);
        if (rc == 0) {
            fprintf(stderr, "[optimize] stored lesson: %s (%d chars)\n",
                    key, body_len);
            n_stored++;
        }
        free(value);

        p = body_end;
        if (strncmp(p, "---", 3) == 0) p += 3;
    }

    free(text);
    fprintf(stderr, "[optimize] generated %d lesson%s from failure patterns\n",
            n_stored, n_stored != 1 ? "s" : "");
    return n_stored;
}

/* ════════════════════════════════════════════════════════
 * Rec #2: Tool Description Optimization [AHE ablation: +3.3pp]
 *
 * Extends the optimizer to also target tool descriptions per model.
 * Tool descriptions are part of the function-calling schema and directly
 * influence how the model selects and uses tools. AHE's evolved harness
 * created a 1364-line shell tool with auto-contextual hints — tool
 * descriptions are the lighter-weight equivalent.
 *
 * The reflection LM outputs a structured format:
 *   === SYSTEM PROMPT ===
 *   <prompt text>
 *   === TOOL DESCRIPTIONS ===
 *   [tool_name] description text
 *   [tool_name] description text
 * ════════════════════════════════════════════════════════ */

char *optimize_parse_proposal(const char *raw_text,
                              char ***out_tool_names,
                              char ***out_tool_descs,
                              int *out_n_tool_descs) {
    *out_tool_names = NULL;
    *out_tool_descs = NULL;
    *out_n_tool_descs = 0;

    if (!raw_text || !raw_text[0]) return NULL;

    /* Look for structured format markers */
    const char *tool_section = strstr(raw_text, "=== TOOL DESCRIPTIONS ===");
    const char *prompt_section = strstr(raw_text, "=== SYSTEM PROMPT ===");

    /* If no structured markers, treat entire text as system prompt (backwards compat) */
    if (!tool_section && !prompt_section)
        return strdup(raw_text);

    /* Extract system prompt portion */
    char *prompt_text = NULL;
    if (prompt_section) {
        const char *start = prompt_section + strlen("=== SYSTEM PROMPT ===");
        while (*start == '\n') start++;

        const char *end = tool_section ? tool_section : (raw_text + strlen(raw_text));
        while (end > start && (end[-1] == '\n' || end[-1] == ' ')) end--;
        if (end > start)
            prompt_text = strndup(start, end - start);
    } else {
        /* No prompt section marker — everything before tool section is prompt */
        const char *end = tool_section;
        while (end > raw_text && (end[-1] == '\n' || end[-1] == ' ')) end--;
        if (end > raw_text)
            prompt_text = strndup(raw_text, end - raw_text);
    }

    /* Parse tool descriptions */
    if (tool_section) {
        const char *p = tool_section + strlen("=== TOOL DESCRIPTIONS ===");
        while (*p == '\n') p++;

        int cap = 8;
        char **names = calloc(cap, sizeof(char *));
        char **descs = calloc(cap, sizeof(char *));
        int n = 0;

        while (*p) {
            /* Expect [tool_name] description */
            if (*p != '[') { p++; continue; }
            p++; /* skip '[' */
            const char *name_end = strchr(p, ']');
            if (!name_end) break;

            char *name = strndup(p, name_end - p);
            p = name_end + 1;
            while (*p == ' ') p++;

            /* Description runs to next '[' at line start, or end */
            const char *desc_end = p;
            while (*desc_end) {
                if (*desc_end == '\n' && desc_end[1] == '[') {
                    break;
                }
                desc_end++;
            }

            /* Trim trailing whitespace */
            while (desc_end > p && (desc_end[-1] == '\n' || desc_end[-1] == ' '))
                desc_end--;

            if (desc_end > p) {
                if (n >= cap) {
                    int new_cap = cap * 2;
                    if (safe_realloc((void **)&names, (size_t)new_cap * sizeof(char *)) ||
                        safe_realloc((void **)&descs, (size_t)new_cap * sizeof(char *))) {
                        free(name);
                        break;
                    }
                    cap = new_cap;
                }
                names[n] = name;
                descs[n] = strndup(p, desc_end - p);
                n++;
            } else {
                free(name);
            }

            p = desc_end;
            while (*p == '\n') p++;
        }

        if (n > 0) {
            *out_tool_names = names;
            *out_tool_descs = descs;
            *out_n_tool_descs = n;
        } else {
            free(names);
            free(descs);
        }
    }

    return prompt_text ? prompt_text : strdup("");
}

int optimize_write_tool_descs_to_profile(const char *profile_path,
                                          char **tool_names,
                                          char **tool_descs,
                                          int n_tool_descs) {
    if (!profile_path || !tool_names || n_tool_descs <= 0) return -1;

    char *data = slurp_file(profile_path, NULL);
    if (!data) return -1;

    str_t out = str_new(strlen(data) + 2048);

    /* Remove any existing [tools.*] sections */
    const char *p = data;
    while (*p) {
        /* Check for [tools.X] section header */
        if (p[0] == '[' && strncmp(p, "[tools.", 7) == 0) {
            /* Skip until next section or end */
            const char *next = p + 1;
            while (*next) {
                if (*next == '\n' && next[1] == '[') {
                    next++;
                    break;
                }
                next++;
            }
            p = next;
            continue;
        }
        /* Copy character */
        const char *nl = strchr(p, '\n');
        if (nl) {
            str_append(&out, p, nl - p + 1);
            p = nl + 1;
        } else {
            str_append_cstr(&out, p);
            break;
        }
    }

    /* Append new tool description sections */
    if (out.len > 0 && out.data[out.len - 1] != '\n')
        str_append_cstr(&out, "\n");

    for (int i = 0; i < n_tool_descs; i++) {
        str_appendf(&out, "\n[tools.%s]\ndescription = \"\"\"\n%s\n\"\"\"\n",
                    tool_names[i], tool_descs[i]);
    }

    free(data);
    int rc = write_file(profile_path, str_cstr(&out), out.len);
    str_free(&out);

    if (rc == 0)
        fprintf(stderr, "[optimize] wrote %d tool description override%s to %s\n",
                n_tool_descs, n_tool_descs > 1 ? "s" : "", profile_path);
    return rc;
}

/* ════════════════════════════════════════════════════════
 * Rec #7: Decision Manifest [AHE Decision Observability]
 *
 * Each harness edit is paired with a falsifiable prediction:
 *   - EXPECT_FIX: query IDs the proposer believes will be fixed
 *   - AT_RISK: query IDs that might regress
 *
 * After evaluation, predictions are verified against actual flips.
 * Prediction accuracy is fed back to the proposer in the next round,
 * closing the observation-decision-verification loop.
 *
 * Based on AHE paper §3.3: "Every edit is paired with self-declared
 * prediction, verified against next round outcomes."
 * ════════════════════════════════════════════════════════ */

manifest_entry_t optimize_parse_manifest(const char *proposal_text) {
    manifest_entry_t m = {0};
    if (!proposal_text) return m;

    /* Parse EXPECT_FIX: q1, q2, q3 */
    const char *fix = strstr(proposal_text, "EXPECT_FIX:");
    if (fix) {
        fix += 11;
        while (*fix == ' ') fix++;
        const char *eol = strchr(fix, '\n');
        if (!eol) eol = fix + strlen(fix);

        int cap = 8;
        m.expect_fix = calloc(cap, sizeof(char *));
        const char *p = fix;
        while (p < eol) {
            while (p < eol && (*p == ' ' || *p == ',')) p++;
            const char *start = p;
            while (p < eol && *p != ',' && *p != '\n') p++;
            const char *end = p;
            while (end > start && end[-1] == ' ') end--;
            if (end > start) {
                if (m.n_expect_fix >= cap) {
                    int new_cap = cap * 2;
                    if (safe_realloc((void **)&m.expect_fix, (size_t)new_cap * sizeof(char *)))
                        break;
                    cap = new_cap;
                }
                m.expect_fix[m.n_expect_fix++] = strndup(start, end - start);
            }
        }
    }

    /* Parse AT_RISK: q1, q2 */
    const char *risk = strstr(proposal_text, "AT_RISK:");
    if (risk) {
        risk += 8;
        while (*risk == ' ') risk++;
        const char *eol = strchr(risk, '\n');
        if (!eol) eol = risk + strlen(risk);

        int cap = 8;
        m.at_risk = calloc(cap, sizeof(char *));
        const char *p = risk;
        while (p < eol) {
            while (p < eol && (*p == ' ' || *p == ',')) p++;
            const char *start = p;
            while (p < eol && *p != ',' && *p != '\n') p++;
            const char *end = p;
            while (end > start && end[-1] == ' ') end--;
            if (end > start) {
                if (m.n_at_risk >= cap) {
                    int new_cap = cap * 2;
                    if (safe_realloc((void **)&m.at_risk, (size_t)new_cap * sizeof(char *)))
                        break;
                    cap = new_cap;
                }
                m.at_risk[m.n_at_risk++] = strndup(start, end - start);
            }
        }
    }

    return m;
}

void optimize_verify_manifest(manifest_entry_t *manifest,
                              const query_flip_t *flips, int n_flips,
                              int n_fixes, int n_regressions) {
    (void)n_fixes;
    (void)n_regressions;
    if (!manifest) return;
    manifest->verified = 1;
    manifest->correct_fixes = 0;
    manifest->missed_fixes = 0;
    manifest->correct_risks = 0;
    manifest->missed_risks = 0;

    /* Check predicted fixes against actual fixes */
    for (int i = 0; i < manifest->n_expect_fix; i++) {
        int found = 0;
        for (int j = 0; j < n_flips; j++) {
            if (flips[j].now_pass && flips[j].query_id &&
                strcmp(flips[j].query_id, manifest->expect_fix[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) manifest->correct_fixes++;
    }

    /* Count actual fixes not in predictions */
    for (int j = 0; j < n_flips; j++) {
        if (!flips[j].now_pass) continue;
        int predicted = 0;
        for (int i = 0; i < manifest->n_expect_fix; i++) {
            if (flips[j].query_id &&
                strcmp(flips[j].query_id, manifest->expect_fix[i]) == 0) {
                predicted = 1;
                break;
            }
        }
        if (!predicted) manifest->missed_fixes++;
    }

    /* Check predicted at-risk against actual regressions */
    for (int i = 0; i < manifest->n_at_risk; i++) {
        int found = 0;
        for (int j = 0; j < n_flips; j++) {
            if (!flips[j].now_pass && flips[j].query_id &&
                strcmp(flips[j].query_id, manifest->at_risk[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) manifest->correct_risks++;
    }

    /* Count actual regressions not in predictions */
    for (int j = 0; j < n_flips; j++) {
        if (flips[j].now_pass) continue;
        int predicted = 0;
        for (int i = 0; i < manifest->n_at_risk; i++) {
            if (flips[j].query_id &&
                strcmp(flips[j].query_id, manifest->at_risk[i]) == 0) {
                predicted = 1;
                break;
            }
        }
        if (!predicted) manifest->missed_risks++;
    }
}

char *optimize_format_manifest_feedback(const manifest_entry_t *manifest) {
    if (!manifest || !manifest->verified) return NULL;

    str_t fb = str_new(512);
    str_append_cstr(&fb, "PREDICTION ACCURACY (from previous round's manifest):\n");

    int total_fix_pred = manifest->n_expect_fix;
    int total_risk_pred = manifest->n_at_risk;

    if (total_fix_pred > 0)
        str_appendf(&fb, "  Fix predictions: %d/%d correct (%.0f%% precision)\n",
                    manifest->correct_fixes, total_fix_pred,
                    total_fix_pred > 0
                        ? 100.0 * manifest->correct_fixes / total_fix_pred : 0.0);

    if (manifest->missed_fixes > 0)
        str_appendf(&fb, "  Unpredicted fixes: %d (recall gap)\n",
                    manifest->missed_fixes);

    if (total_risk_pred > 0)
        str_appendf(&fb, "  Risk predictions: %d/%d materialized (%.0f%% precision)\n",
                    manifest->correct_risks, total_risk_pred,
                    total_risk_pred > 0
                        ? 100.0 * manifest->correct_risks / total_risk_pred : 0.0);

    if (manifest->missed_risks > 0)
        str_appendf(&fb, "  Unpredicted regressions: %d (BLIND SPOT — improve risk prediction)\n",
                    manifest->missed_risks);

    str_append_cstr(&fb,
        "Use this feedback to improve prediction accuracy in your next proposal.\n\n");

    return str_steal(&fb);
}

void optimize_free_manifest(manifest_entry_t *m) {
    if (!m) return;
    free_string_array(m->expect_fix, m->n_expect_fix);
    free_string_array(m->at_risk, m->n_at_risk);
    memset(m, 0, sizeof(*m));
}
