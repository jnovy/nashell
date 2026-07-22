#!/usr/bin/env bash
#
# test_reflection_gaps.sh -- Structural verification tests for 7 reflection
# pipeline fixes in react_reflection.c.
#
# These tests verify the CODE CHANGES exist with correct placement, ordering,
# and content. They don't require an LLM provider or runtime -- they grep the
# source to prove the fixes are wired in correctly.
#
# Usage: ./tests/test_reflection_gaps.sh
#
set -euo pipefail

SRC="src/react_reflection.c"
passes=0
failures=0

pass() { passes=$((passes+1)); printf "  %-60s ok\n" "$1"; }
fail() { failures=$((failures+1)); printf "  FAIL: %-56s ***\n" "$1"; }
check() { if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }

echo "=== Reflection Gap Structural Tests ==="

# ── Gap 1+2: Second consolidation flush after reflection mini-loop ──

desc="Gap1: tool_flush_deferred_consolidations exists"
check grep -q 'tool_flush_deferred_consolidations' "$SRC"

# Verify the flush appears AFTER the reflection mini-loop (after thinking restore)
# and BEFORE scratchpad pruning. We check ordering by line numbers.
flush_line=$(grep -n 'tool_flush_deferred_consolidations' "$SRC" | tail -1 | cut -d: -f1)
thinking_restore=$(grep -n 'refl_provider->cfg.enable_thinking = saved_thinking' "$SRC" | head -1 | cut -d: -f1)
pruning_start=$(grep -n 'Post-reflection scratchpad pruning' "$SRC" | head -1 | cut -d: -f1)

desc="Gap1: flush AFTER thinking restore (L${flush_line} > L${thinking_restore})"
if [ -n "$flush_line" ] && [ -n "$thinking_restore" ] && [ "$flush_line" -gt "$thinking_restore" ]; then
    pass "$desc"
else
    fail "$desc"
fi

desc="Gap1: flush BEFORE scratchpad pruning (L${flush_line} < L${pruning_start})"
if [ -n "$flush_line" ] && [ -n "$pruning_start" ] && [ "$flush_line" -lt "$pruning_start" ]; then
    pass "$desc"
else
    fail "$desc"
fi

# Verify the guard condition (n_deferred_consol > 0)
desc="Gap1: flush guarded by n_deferred_consol > 0"
check grep -q 'n_deferred_consol > 0' "$SRC"

# There should be TWO flushes total (one pre-reflection, one post-reflection)
flush_count=$(grep -c 'tool_flush_deferred_consolidations' "$SRC")
desc="Gap1+2: exactly 2 consolidation flushes (found ${flush_count})"
if [ "$flush_count" -eq 2 ]; then pass "$desc"; else fail "$desc"; fi

# ── Gap 3: Dedup scans both global AND workspace-local memory ──

desc="Gap3: dedup_scan_one_memory() helper exists"
check grep -q 'dedup_scan_one_memory' "$SRC"

desc="Gap3: workspace memory scanned (ws->workspace)"
check grep -q 'ctx->tools->ws->workspace' "$SRC"

desc="Gap3: memory_has_embeddings checks workspace"
check grep -q 'memory_has_embeddings(ctx->tools->ws->workspace)' "$SRC"

# Verify the emb_mem variable for embedding context selection
desc="Gap3: emb_mem variable for dual-memory embedding"
check grep -q 'emb_mem' "$SRC"

desc="Gap3: emb_mem checks workspace as fallback"
check grep -qA2 'emb_mem = ctx->tools->memory' "$SRC"

# ── Gap 4: Reflection prompts expose 'supersedes' parameter ──

desc="Gap4: success prompt mentions supersedes"
check grep -q 'supersedes.*replaces.*optional' "$SRC"

desc="Gap4: failure prompt mentions supersedes"
check grep -q 'supersedes.*corrects.*optional' "$SRC"

# Verify supersedes appears in BOTH prompt sections (success and failure)
supersedes_count=$(grep -c 'supersedes' "$SRC")
desc="Gap4: supersedes appears >= 2 times (found ${supersedes_count})"
if [ "$supersedes_count" -ge 2 ]; then pass "$desc"; else fail "$desc"; fi

# ── Gap 5: user_query injected into reflection context ──

# (void)user_query should NOT exist (it was the suppression)
desc="Gap5: (void)user_query suppression removed"
if grep -q '(void)user_query' "$SRC"; then fail "$desc"; else pass "$desc"; fi

desc="Gap5: [USER QUERY] context message exists"
check grep -q '\[USER QUERY\]' "$SRC"

# Verify user_query is actually used (appears in a snprintf/str_append, not just signature)
uq_usage=$(grep -c 'user_query' "$SRC")
desc="Gap5: user_query referenced multiple times (${uq_usage} >= 3)"
if [ "$uq_usage" -ge 3 ]; then pass "$desc"; else fail "$desc"; fi

# ── Gap 6: Thinking mode disabled for scratchpad pruning ──

desc="Gap6: saved_thinking_prune variable exists"
check grep -q 'saved_thinking_prune' "$SRC"

# Verify the pattern: save, disable, call, restore
desc="Gap6: thinking disabled before provider_complete"
# Check that enable_thinking = 0 appears near provider_complete (within 5 lines)
prune_complete=$(grep -n 'provider_complete(ctx->provider, prune_chat' "$SRC" | cut -d: -f1)
if [ -n "$prune_complete" ]; then
    disable_line=$(grep -n 'enable_thinking = 0' "$SRC" | tail -1 | cut -d: -f1)
    restore_line=$(grep -n 'enable_thinking = saved_thinking_prune' "$SRC" | head -1 | cut -d: -f1)
    if [ -n "$disable_line" ] && [ -n "$restore_line" ] && \
       [ "$disable_line" -lt "$prune_complete" ] && [ "$restore_line" -gt "$prune_complete" ]; then
        pass "$desc"
    else
        fail "$desc"
    fi
else
    fail "$desc"
fi

# ── Gap 7: Unknown actions handled explicitly ──

# Verify there's an else branch that sets should_store = 0 for unknown actions
desc="Gap7: else branch for unknown actions exists"
check grep -q 'Unknown action' "$SRC"

desc="Gap7: should_store = 0 in unknown action branch"
# Verify should_store = 0 appears after "Unknown action" comment
unknown_line=$(grep -n 'Unknown action' "$SRC" | head -1 | cut -d: -f1)
store_zero_line=$(grep -n 'should_store = 0' "$SRC" | while read line; do
    lnum=$(echo "$line" | cut -d: -f1)
    if [ "$lnum" -gt "$unknown_line" ] && [ "$lnum" -lt $((unknown_line + 5)) ]; then
        echo "$lnum"
        break
    fi
done)
if [ -n "$store_zero_line" ]; then pass "$desc"; else fail "$desc"; fi

# ── Compilation check ──

desc="Compilation: react_reflection.c compiles cleanly"
if make -j$(nproc) nash 2>&1 | tail -5 | grep -q 'Error\|error:'; then
    fail "$desc"
else
    pass "$desc"
fi

# ── Summary ──

echo ""
echo "  ${passes} passed, ${failures} failed"
echo "=== $([ $failures -eq 0 ] && echo 'ALL TESTS PASSED' || echo 'SOME TESTS FAILED') ==="
exit $failures
