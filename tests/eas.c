/*
 * eas.c — Entropy Area Score (EAS) for reasoning LLMs
 *
 * Implements the metric from:
 *   "Uncertainty Under the Curve: A Sequence-Level Entropy Area Metric
 *    for Reasoning LLM" — Zhu et al., arXiv:2508.20384
 *
 * EAS(S) = Σ_{t=1}^{T-1} H_t
 *
 * where H_t = −Σ_{v} P_t(v) · log₂ P_t(v) is the Shannon entropy of
 * the predictive distribution at position t.
 *
 * In practice, only the top-K token probabilities are needed per position
 * (K=20 captures >99.87% of mass, introducing <0.031 bits error).
 *
 * Compile: gcc -Wall -Wextra -O2 -o eas eas.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

/* ---------- data types -------------------------------------------------- */

/*
 * Per-position probability distribution (top-K tokens only).
 * probs[i] is the probability of the i-th most likely token.
 * k is how many entries are populated (1..top_k).
 */
typedef struct {
    int      k;        /* number of top-K entries for this position  */
    double  *probs;    /* array of k probabilities (must sum to ~1)  */
} token_dist_t;

/* ---------- core functions ---------------------------------------------- */

/*
 * Shannon entropy in bits for a single position's distribution.
 * Skips zero-probability entries (0·log₂0 = 0 by convention).
 */
static double token_entropy(const token_dist_t *dist)
{
    double h = 0.0;
    for (int i = 0; i < dist->k; i++) {
        double p = dist->probs[i];
        if (p > 0.0)
            h -= p * log2(p);
    }
    return h;
}

/*
 * Entropy Area Score — sum of per-position entropies.
 *
 *   positions:  array of T token distributions (one per generation step)
 *   T:          number of positions
 *
 * Returns EAS ≥ 0.  Returns -1.0 on invalid input.
 */
double eas(const token_dist_t *positions, int T)
{
    if (!positions || T <= 0)
        return -1.0;

    double score = 0.0;
    for (int t = 0; t < T; t++)
        score += token_entropy(&positions[t]);
    return score;
}

/*
 * Mean Entropy Area Score — length-normalized variant.
 *
 * Mean EAS = EAS / T
 *
 * Returns -1.0 on invalid input.
 */
double eas_mean(const token_dist_t *positions, int T)
{
    double total = eas(positions, T);
    if (total < 0.0)
        return -1.0;
    return total / (double)T;
}

/*
 * Convenience: compute EAS directly from a flat array of log-probabilities.
 *
 *   logprobs:  flat array of size T * top_k (row-major, base-e log probs)
 *   T:         number of positions
 *   top_k:     tokens per position (constant across positions)
 *
 * Each row logprobs[t*top_k .. t*top_k + top_k - 1] holds the top-K
 * log-probabilities for position t.  Internally converts to probabilities
 * via exp().
 *
 * Returns EAS ≥ 0, or -1.0 on error.
 */
double eas_from_logprobs(const double *logprobs, int T, int top_k)
{
    if (!logprobs || T <= 0 || top_k <= 0)
        return -1.0;

    double score = 0.0;

    for (int t = 0; t < T; t++) {
        double h = 0.0;
        for (int i = 0; i < top_k; i++) {
            double p = exp(logprobs[t * top_k + i]);
            if (p > 0.0)
                h -= p * log2(p);
        }
        score += h;
    }
    return score;
}

/* ---------- self-tests -------------------------------------------------- */

#ifdef EAS_TEST   /* compile with -DEAS_TEST to include tests */

static int g_pass = 0, g_fail = 0;

#define CHECK(name, cond) do {                                      \
    if (cond) { g_pass++; printf("  PASS  %s\n", name); }          \
    else      { g_fail++; printf("  FAIL  %s\n", name); }          \
} while (0)

#define NEAR(a, b, eps) (fabs((a) - (b)) < (eps))

static void test_single_token_certain(void)
{
    /* Deterministic position: prob = 1.0 → entropy = 0 */
    double p[] = {1.0};
    token_dist_t d = {1, p};
    CHECK("single_certain", NEAR(token_entropy(&d), 0.0, 1e-12));
}

static void test_uniform_2(void)
{
    /* Uniform over 2 tokens → entropy = 1 bit */
    double p[] = {0.5, 0.5};
    token_dist_t d = {2, p};
    CHECK("uniform_2", NEAR(token_entropy(&d), 1.0, 1e-12));
}

static void test_uniform_4(void)
{
    /* Uniform over 4 tokens → entropy = 2 bits */
    double p[] = {0.25, 0.25, 0.25, 0.25};
    token_dist_t d = {4, p};
    CHECK("uniform_4", NEAR(token_entropy(&d), 2.0, 1e-12));
}

static void test_eas_basic(void)
{
    /*
     * 3 positions:
     *   pos 0: certain      → H = 0
     *   pos 1: uniform(2)   → H = 1
     *   pos 2: uniform(4)   → H = 2
     * EAS = 0 + 1 + 2 = 3
     */
    double p0[] = {1.0};
    double p1[] = {0.5, 0.5};
    double p2[] = {0.25, 0.25, 0.25, 0.25};
    token_dist_t dists[] = {
        {1, p0}, {2, p1}, {4, p2}
    };
    CHECK("eas_basic", NEAR(eas(dists, 3), 3.0, 1e-12));
}

static void test_eas_mean_basic(void)
{
    double p0[] = {1.0};
    double p1[] = {0.5, 0.5};
    double p2[] = {0.25, 0.25, 0.25, 0.25};
    token_dist_t dists[] = {
        {1, p0}, {2, p1}, {4, p2}
    };
    CHECK("eas_mean_basic", NEAR(eas_mean(dists, 3), 1.0, 1e-12));
}

static void test_eas_invalid(void)
{
    CHECK("eas_null",     NEAR(eas(NULL, 5), -1.0, 1e-12));
    CHECK("eas_zero_T",   NEAR(eas(NULL, 0), -1.0, 1e-12));
}

static void test_logprobs_basic(void)
{
    /*
     * 2 positions, top_k = 2
     *   pos 0: log(0.5), log(0.5)  → H = 1 bit
     *   pos 1: log(1.0), log(0.0)  → H = 0 bits  (log(0) = -inf, exp(-inf)=0)
     * EAS = 1.0
     *
     * Note: log(0) = -inf, exp(-inf) = 0.0, 0·log₂(0) = 0 by convention.
     */
    double lp[] = {
        log(0.5), log(0.5),
        0.0,      -INFINITY
    };
    CHECK("logprobs_basic", NEAR(eas_from_logprobs(lp, 2, 2), 1.0, 1e-12));
}

static void test_logprobs_invalid(void)
{
    CHECK("logprobs_null", NEAR(eas_from_logprobs(NULL, 2, 2), -1.0, 1e-12));
    CHECK("logprobs_bad_k", NEAR(eas_from_logprobs(NULL, 2, 0), -1.0, 1e-12));
}

static void test_skewed_distribution(void)
{
    /* p = {0.9, 0.1} → H = -0.9·log₂(0.9) - 0.1·log₂(0.1) ≈ 0.469 bits */
    double p[] = {0.9, 0.1};
    token_dist_t d = {2, p};
    double expected = -(0.9 * log2(0.9) + 0.1 * log2(0.1));
    CHECK("skewed_dist", NEAR(token_entropy(&d), expected, 1e-10));
}

static void test_zero_prob_entries(void)
{
    /* Zero-prob entries should be skipped gracefully */
    double p[] = {0.5, 0.0, 0.5, 0.0};
    token_dist_t d = {4, p};
    CHECK("zero_probs", NEAR(token_entropy(&d), 1.0, 1e-12));
}

static void test_monotone_convergence(void)
{
    /*
     * Simulate a model converging: entropy should decrease over time.
     * pos 0: uniform(8) → H = 3 bits
     * pos 1: uniform(4) → H = 2 bits
     * pos 2: uniform(2) → H = 1 bit
     * pos 3: certain    → H = 0 bits
     * EAS = 6.0
     */
    double p0[] = {0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125};
    double p1[] = {0.25, 0.25, 0.25, 0.25};
    double p2[] = {0.5, 0.5};
    double p3[] = {1.0};
    token_dist_t dists[] = {
        {8, p0}, {4, p1}, {2, p2}, {1, p3}
    };
    double total = eas(dists, 4);
    CHECK("convergence_eas", NEAR(total, 6.0, 1e-12));
    CHECK("convergence_mean", NEAR(eas_mean(dists, 4), 1.5, 1e-12));
}

int main(void)
{
    printf("EAS self-tests:\n");

    test_single_token_certain();
    test_uniform_2();
    test_uniform_4();
    test_skewed_distribution();
    test_zero_prob_entries();
    test_eas_basic();
    test_eas_mean_basic();
    test_eas_invalid();
    test_logprobs_basic();
    test_logprobs_invalid();
    test_monotone_convergence();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* EAS_TEST */
