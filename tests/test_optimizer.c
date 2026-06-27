/* test_optimizer.c -- Tests for per-query regression tracking
 * in the Self-Harness optimizer.
 *
 * Covers: optimize_compare_reports_per_query() and the
 * per-query flip detection logic added for AHE-inspired
 * regression blindness mitigation.
 */

#include "test_common.h"
#include "../src/prompt_optimize.h"

/* ── Helpers to build mock regression reports ─────────── */

static query_result_t make_qr(const char *id, int passed, double score) {
    query_result_t qr;
    memset(&qr, 0, sizeof(qr));
    qr.query_id = strdup(id);
    qr.passed = passed;
    qr.score = score;
    qr.steps_used = 5;
    return qr;
}

static bank_result_t make_bank(const char *name, query_result_t *results, int n) {
    bank_result_t br;
    memset(&br, 0, sizeof(br));
    br.bank_name = strdup(name);
    br.split = SPLIT_HELD_IN;
    br.results = results;
    br.n_results = n;
    br.total = n;
    int passed = 0;
    for (int i = 0; i < n; i++)
        if (results[i].passed) passed++;
    br.passed = passed;
    br.score = n > 0 ? (double)passed / n : 0;
    return br;
}

static regression_report_t *make_report(bank_result_t *banks, int n_banks) {
    regression_report_t *rpt = calloc(1, sizeof(regression_report_t));
    rpt->bank_results = banks;
    rpt->n_bank_results = n_banks;
    int total = 0, passed = 0;
    for (int i = 0; i < n_banks; i++) {
        total += banks[i].total;
        passed += banks[i].passed;
    }
    rpt->total_queries = total;
    rpt->total_passed = passed;
    rpt->overall_score = total > 0 ? (double)passed / total : 0;
    rpt->held_in_score = -1;
    rpt->held_out_score = -1;
    return rpt;
}

static void free_mock_report(regression_report_t *rpt) {
    if (!rpt) return;
    for (int i = 0; i < rpt->n_bank_results; i++) {
        bank_result_t *br = &rpt->bank_results[i];
        for (int j = 0; j < br->n_results; j++)
            free(br->results[j].query_id);
        free(br->bank_name);
    }
    /* Don't free br->results -- they are stack-allocated in the tests */
    free(rpt->bank_results);
    free(rpt);
}

/* ══════════════════════════════════════════════════════════
 * Test: NULL inputs
 * ══════════════════════════════════════════════════════════ */

static void test_null_inputs(void) {
    int n_flips = -1, n_fixes = -1, n_regr = -1;
    query_flip_t *flips = optimize_compare_reports_per_query(
        NULL, NULL, &n_flips, &n_fixes, &n_regr);
    ASSERT_NULL(flips);
    ASSERT_EQ(n_flips, 0);
    ASSERT_EQ(n_fixes, 0);
    ASSERT_EQ(n_regr, 0);
}

/* ══════════════════════════════════════════════════════════
 * Test: no flips (identical results)
 * ══════════════════════════════════════════════════════════ */

static void test_no_flips(void) {
    query_result_t qrs_base[] = {
        make_qr("q1", 1, 1.0),
        make_qr("q2", 0, 0.0),
        make_qr("q3", 1, 0.8),
    };
    query_result_t qrs_cand[] = {
        make_qr("q1", 1, 1.0),
        make_qr("q2", 0, 0.0),
        make_qr("q3", 1, 0.8),
    };
    bank_result_t *banks_base = malloc(sizeof(bank_result_t));
    banks_base[0] = make_bank("test", qrs_base, 3);
    bank_result_t *banks_cand = malloc(sizeof(bank_result_t));
    banks_cand[0] = make_bank("test", qrs_cand, 3);

    regression_report_t *base = make_report(banks_base, 1);
    regression_report_t *cand = make_report(banks_cand, 1);

    int n_flips, n_fixes, n_regr;
    query_flip_t *flips = optimize_compare_reports_per_query(
        base, cand, &n_flips, &n_fixes, &n_regr);

    ASSERT_EQ(n_flips, 0);
    ASSERT_EQ(n_fixes, 0);
    ASSERT_EQ(n_regr, 0);

    free(flips);
    free_mock_report(base);
    free_mock_report(cand);
}

/* ══════════════════════════════════════════════════════════
 * Test: one fix, one regression
 * ══════════════════════════════════════════════════════════ */

static void test_fix_and_regression(void) {
    /* Baseline: q1 passes, q2 fails, q3 passes */
    query_result_t qrs_base[] = {
        make_qr("q1", 1, 1.0),
        make_qr("q2", 0, 0.0),
        make_qr("q3", 1, 0.8),
    };
    /* Candidate: q1 fails (regression!), q2 passes (fix!), q3 passes */
    query_result_t qrs_cand[] = {
        make_qr("q1", 0, 0.3),
        make_qr("q2", 1, 1.0),
        make_qr("q3", 1, 0.8),
    };
    bank_result_t *banks_base = malloc(sizeof(bank_result_t));
    banks_base[0] = make_bank("test", qrs_base, 3);
    bank_result_t *banks_cand = malloc(sizeof(bank_result_t));
    banks_cand[0] = make_bank("test", qrs_cand, 3);

    regression_report_t *base = make_report(banks_base, 1);
    regression_report_t *cand = make_report(banks_cand, 1);

    int n_flips, n_fixes, n_regr;
    query_flip_t *flips = optimize_compare_reports_per_query(
        base, cand, &n_flips, &n_fixes, &n_regr);

    ASSERT_EQ(n_flips, 2);
    ASSERT_EQ(n_fixes, 1);
    ASSERT_EQ(n_regr, 1);
    ASSERT_NOT_NULL(flips);

    /* Find the regression */
    int found_regr = 0, found_fix = 0;
    for (int i = 0; i < n_flips; i++) {
        if (strcmp(flips[i].query_id, "q1") == 0) {
            ASSERT_EQ(flips[i].was_pass, 1);
            ASSERT_EQ(flips[i].now_pass, 0);
            ASSERT(flips[i].delta_score < 0);
            found_regr = 1;
        } else if (strcmp(flips[i].query_id, "q2") == 0) {
            ASSERT_EQ(flips[i].was_pass, 0);
            ASSERT_EQ(flips[i].now_pass, 1);
            ASSERT(flips[i].delta_score > 0);
            found_fix = 1;
        }
    }
    ASSERT(found_regr);
    ASSERT(found_fix);

    free(flips);
    free_mock_report(base);
    free_mock_report(cand);
}

/* ══════════════════════════════════════════════════════════
 * Test: all pass to all fail
 * ══════════════════════════════════════════════════════════ */

static void test_all_regressed(void) {
    query_result_t qrs_base[] = {
        make_qr("q1", 1, 1.0),
        make_qr("q2", 1, 0.9),
    };
    query_result_t qrs_cand[] = {
        make_qr("q1", 0, 0.2),
        make_qr("q2", 0, 0.1),
    };
    bank_result_t *banks_base = malloc(sizeof(bank_result_t));
    banks_base[0] = make_bank("test", qrs_base, 2);
    bank_result_t *banks_cand = malloc(sizeof(bank_result_t));
    banks_cand[0] = make_bank("test", qrs_cand, 2);

    regression_report_t *base = make_report(banks_base, 1);
    regression_report_t *cand = make_report(banks_cand, 1);

    int n_flips, n_fixes, n_regr;
    query_flip_t *flips = optimize_compare_reports_per_query(
        base, cand, &n_flips, &n_fixes, &n_regr);

    ASSERT_EQ(n_flips, 2);
    ASSERT_EQ(n_fixes, 0);
    ASSERT_EQ(n_regr, 2);

    free(flips);
    free_mock_report(base);
    free_mock_report(cand);
}

/* ══════════════════════════════════════════════════════════
 * Test: multiple banks
 * ══════════════════════════════════════════════════════════ */

static void test_multiple_banks(void) {
    /* Bank 1: q1 pass->fail */
    query_result_t qrs_base1[] = { make_qr("q1", 1, 1.0) };
    query_result_t qrs_cand1[] = { make_qr("q1", 0, 0.2) };
    /* Bank 2: q2 fail->pass */
    query_result_t qrs_base2[] = { make_qr("q2", 0, 0.1) };
    query_result_t qrs_cand2[] = { make_qr("q2", 1, 0.9) };

    bank_result_t *banks_base = malloc(2 * sizeof(bank_result_t));
    banks_base[0] = make_bank("bank1", qrs_base1, 1);
    banks_base[1] = make_bank("bank2", qrs_base2, 1);
    bank_result_t *banks_cand = malloc(2 * sizeof(bank_result_t));
    banks_cand[0] = make_bank("bank1", qrs_cand1, 1);
    banks_cand[1] = make_bank("bank2", qrs_cand2, 1);

    regression_report_t *base = make_report(banks_base, 2);
    regression_report_t *cand = make_report(banks_cand, 2);

    int n_flips, n_fixes, n_regr;
    query_flip_t *flips = optimize_compare_reports_per_query(
        base, cand, &n_flips, &n_fixes, &n_regr);

    ASSERT_EQ(n_flips, 2);
    ASSERT_EQ(n_fixes, 1);
    ASSERT_EQ(n_regr, 1);

    free(flips);
    free_mock_report(base);
    free_mock_report(cand);
}

/* ══════════════════════════════════════════════════════════
 * Test: query in candidate not in baseline (new query)
 * ══════════════════════════════════════════════════════════ */

static void test_new_query_ignored(void) {
    query_result_t qrs_base[] = {
        make_qr("q1", 1, 1.0),
    };
    query_result_t qrs_cand[] = {
        make_qr("q1", 1, 1.0),
        make_qr("q_new", 0, 0.0),  /* not in baseline */
    };
    bank_result_t *banks_base = malloc(sizeof(bank_result_t));
    banks_base[0] = make_bank("test", qrs_base, 1);
    bank_result_t *banks_cand = malloc(sizeof(bank_result_t));
    banks_cand[0] = make_bank("test", qrs_cand, 2);

    regression_report_t *base = make_report(banks_base, 1);
    regression_report_t *cand = make_report(banks_cand, 1);

    int n_flips, n_fixes, n_regr;
    query_flip_t *flips = optimize_compare_reports_per_query(
        base, cand, &n_flips, &n_fixes, &n_regr);

    ASSERT_EQ(n_flips, 0);  /* new query ignored since not in baseline */
    ASSERT_EQ(n_fixes, 0);
    ASSERT_EQ(n_regr, 0);

    free(flips);
    free_mock_report(base);
    free_mock_report(cand);
}

/* ══════════════════════════════════════════════════════════
 * Test: empty reports
 * ══════════════════════════════════════════════════════════ */

static void test_empty_reports(void) {
    bank_result_t *banks = malloc(sizeof(bank_result_t));
    memset(banks, 0, sizeof(bank_result_t));
    banks[0].bank_name = strdup("empty");

    regression_report_t *base = make_report(banks, 1);

    bank_result_t *banks2 = malloc(sizeof(bank_result_t));
    memset(banks2, 0, sizeof(bank_result_t));
    banks2[0].bank_name = strdup("empty");

    regression_report_t *cand = make_report(banks2, 1);

    int n_flips, n_fixes, n_regr;
    query_flip_t *flips = optimize_compare_reports_per_query(
        base, cand, &n_flips, &n_fixes, &n_regr);

    ASSERT_NULL(flips);
    ASSERT_EQ(n_flips, 0);

    free_mock_report(base);
    free_mock_report(cand);
}

/* ── Main ─────────────────────────────────────────────── */

int main(void) {
    printf("test_optimizer:\n");
    RUN_TEST(test_null_inputs);
    RUN_TEST(test_no_flips);
    RUN_TEST(test_fix_and_regression);
    RUN_TEST(test_all_regressed);
    RUN_TEST(test_multiple_banks);
    RUN_TEST(test_new_query_ignored);
    RUN_TEST(test_empty_reports);
    TEST_SUMMARY();
}
