#!/usr/bin/env bash
#
# test_coding_react.sh — Nash coding task test suite
#
# Runs nash in headless mode (-p) with a coding task, then runs two
# follow-up react loops (--session) to refine/extend the code.
# Repeats N times and analyzes results across all runs.
#
# Usage:
#   ./tests/test_coding_react.sh [--runs N] [--api URL] [--max-steps S]
#
# Defaults:
#   --runs 3        Number of complete test sequences
#   --api http://192.168.1.18:8080
#   --max-steps 0   (unlimited, inherits from config)

set -euo pipefail

# ── Configuration ──────────────────────────────────────────────
NASH_BIN="${NASH_BIN:-$(dirname "$0")/../nash}"
API_URL="http://192.168.1.18:8080"
NUM_RUNS=3
MAX_STEPS=""  # empty = use config default
VERBOSE=0

# Parse CLI arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs)     NUM_RUNS="$2"; shift 2 ;;
        --api)      API_URL="$2"; shift 2 ;;
        --max-steps) MAX_STEPS="$2"; shift 2 ;;
        --verbose)  VERBOSE=1; shift ;;
        --help)
            echo "Usage: $0 [--runs N] [--api URL] [--max-steps S] [--verbose]"
            exit 0
            ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Resolve absolute path to nash binary
NASH_BIN="$(realpath "$NASH_BIN")"
if [[ ! -x "$NASH_BIN" ]]; then
    echo "ERROR: Nash binary not found or not executable: $NASH_BIN"
    exit 1
fi

# ── Coding task prompts ────────────────────────────────────────
# Initial task: write a small but non-trivial program
PROMPT_INITIAL='Write a C program called "wordfreq.c" that reads text from stdin, counts the frequency of each word (case-insensitive, ignoring punctuation), and prints the top 10 most frequent words sorted by frequency (descending), then alphabetically for ties. Each output line should be: "COUNT WORD". The program should handle large inputs efficiently using a hash table. Write the file to /tmp/nash_test_workspace/wordfreq.c and compile it with gcc.'

# Follow-up 1: add a feature
PROMPT_FOLLOWUP1='Now extend wordfreq.c to also accept an optional command-line argument "-n NUM" that changes the number of top words to display (default remains 10). Also add a "-i FILE" option to read from a file instead of stdin. Update the code, recompile, and test it by creating a sample text file and running the program on it.'

# Follow-up 2: fix bugs and add tests
PROMPT_FOLLOWUP2='Review the wordfreq.c code for any bugs or edge cases (empty input, very long words, special characters, memory leaks). Fix any issues found. Then write a small test script /tmp/nash_test_workspace/test_wordfreq.sh that exercises at least 5 different test cases (empty input, single word repeated, punctuation handling, case insensitivity, -n flag, -i flag) and reports PASS/FAIL for each. Run the test script and report results.'

# ── Color helpers ──────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'  # No Color

log_info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_header(){ echo -e "\n${BOLD}${CYAN}═══ $* ═══${NC}"; }

# ── Helper: run one nash invocation ────────────────────────────
# Usage: run_nash DATA_DIR PROMPT [SESSION_DIR]
# Sets: NASH_EXIT, NASH_STDOUT, NASH_STDERR, NASH_DURATION, NASH_SESSION_DIR
run_nash() {
    local data_dir="$1"
    local prompt="$2"
    local session_dir="${3:-}"

    local args=( --api "$API_URL" --data-dir "$data_dir" -p "$prompt" )
    if [[ -n "$session_dir" ]]; then
        args+=( --session "$session_dir" )
    fi

    local start_time
    start_time=$(date +%s.%N)

    local stdout_file stderr_file
    stdout_file=$(mktemp)
    stderr_file=$(mktemp)

    set +e
    "$NASH_BIN" "${args[@]}" >"$stdout_file" 2>"$stderr_file"
    NASH_EXIT=$?
    set -e

    local end_time
    end_time=$(date +%s.%N)
    NASH_DURATION=$(echo "$end_time - $start_time" | bc)

    NASH_STDOUT=$(cat "$stdout_file")
    NASH_STDERR=$(cat "$stderr_file" | strip_ansi)

    # Find the session directory (most recent in data_dir/sessions/)
    if [[ -z "$session_dir" ]]; then
        NASH_SESSION_DIR=$(ls -1td "$data_dir/sessions/"*/ 2>/dev/null | head -1)
    else
        NASH_SESSION_DIR="$session_dir"
    fi

    rm -f "$stdout_file" "$stderr_file"
}

# ── Helper: strip ANSI escape codes and carriage returns ───────
strip_ansi() {
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g; s/\r//g'
}

# ── Helper: extract metrics from stderr ────────────────────────
# Parses [step N] lines and token stats from nash stderr output
# Note: Nash stderr contains ANSI escape codes (\r\033[K) that must
# be stripped before pattern matching.
parse_stderr_metrics() {
    local stderr_text="$1"
    # Note: ANSI codes already stripped in run_nash()

    # Count completed steps ("[step N] tool: desc (duration)")
    # Excludes "[step N/M] thinking..." lines (those are step-start, not complete)
    METRIC_STEPS=$(echo "$stderr_text" | grep -cP '\[step \d+\] \w+:' || true)

    # Count tool calls by type
    METRIC_SHELL_EXEC=$(echo "$stderr_text" | grep -c 'shell_exec:' || true)
    METRIC_FILE_READ=$(echo "$stderr_text" | grep -c 'file_read:' || true)
    METRIC_FILE_WRITE=$(echo "$stderr_text" | grep -c 'file_write:' || true)
    METRIC_FILE_EDIT=$(echo "$stderr_text" | grep -c 'file_edit:' || true)
    METRIC_GREP_SEARCH=$(echo "$stderr_text" | grep -c 'grep_search:' || true)
    METRIC_DONE=$(echo "$stderr_text" | grep -c '\] done' || true)

    # Extract token counts from the final stats line
    # Format: [PROMPT→COMPLETION tok | pp X t/s | gen Y t/s | ctx Z% | total TIME]
    local stats_line
    stats_line=$(echo "$stderr_text" | grep -oP '\[\d+→\d+ tok[^\]]*\]' | tail -1 || true)
    if [[ -n "$stats_line" ]]; then
        METRIC_PROMPT_TOKENS=$(echo "$stats_line" | grep -oP '^\[\K\d+' || echo "0")
        METRIC_COMPLETION_TOKENS=$(echo "$stats_line" | grep -oP '→\K\d+' || echo "0")
        METRIC_PP_SPEED=$(echo "$stats_line" | grep -oP 'pp \K[0-9.]+' || echo "0")
        METRIC_GEN_SPEED=$(echo "$stats_line" | grep -oP 'gen \K[0-9.]+' || echo "0")
        METRIC_CTX_PCT=$(echo "$stats_line" | grep -oP 'ctx \K[0-9]+' || echo "0")
    else
        METRIC_PROMPT_TOKENS=0
        METRIC_COMPLETION_TOKENS=0
        METRIC_PP_SPEED=0
        METRIC_GEN_SPEED=0
        METRIC_CTX_PCT=0
    fi

    # Count errors and warnings
    METRIC_ERRORS=$(echo "$stderr_text" | grep -c '\[error\]' || true)
    METRIC_WARNINGS=$(echo "$stderr_text" | grep -c '\[warning\]' || true)
}

# ── Helper: analyze journal.jsonl ──────────────────────────────
parse_journal() {
    local journal_path="$1"
    local react_loop="${2:-}"

    if [[ ! -f "$journal_path" ]]; then
        JOURNAL_STEPS=0
        JOURNAL_TOOLS=""
        JOURNAL_ERRORS=0
        return
    fi

    local filter=""
    if [[ -n "$react_loop" ]]; then
        filter="select(.react_loop == $react_loop) |"
    fi

    # Total steps in this react loop
    JOURNAL_STEPS=$(jq -r "$filter .step" "$journal_path" 2>/dev/null | sort -n | tail -1 || echo "0")

    # Tool usage breakdown
    JOURNAL_TOOLS=$(jq -r "$filter .tool // empty" "$journal_path" 2>/dev/null | sort | uniq -c | sort -rn || true)

    # Count failed tool calls
    JOURNAL_ERRORS=$(jq -r "$filter select(.failed == true) | .tool" "$journal_path" 2>/dev/null | wc -l || echo "0")

    # Count react loops present
    JOURNAL_REACT_LOOPS=$(jq -r '.react_loop' "$journal_path" 2>/dev/null | sort -nu | wc -l || echo "0")
}

# ── Helper: check code quality outcomes ────────────────────────
check_outcomes() {
    local workspace="$1"

    OUTCOME_FILE_EXISTS=0
    OUTCOME_COMPILES=0
    OUTCOME_RUNS=0
    OUTCOME_TESTS_EXIST=0
    OUTCOME_TESTS_PASS=0
    OUTCOME_TEST_RESULTS=""

    # Check if wordfreq.c was created
    if [[ -f "$workspace/wordfreq.c" ]]; then
        OUTCOME_FILE_EXISTS=1

        # Check if it compiles
        if gcc -o "$workspace/wordfreq" "$workspace/wordfreq.c" 2>/dev/null; then
            OUTCOME_COMPILES=1

            # Check if it runs on basic input
            local run_output
            if run_output=$(echo "hello world hello" | timeout 5 "$workspace/wordfreq" 2>/dev/null); then
                OUTCOME_RUNS=1
            fi
        fi
    fi

    # Check if test script was created
    if [[ -f "$workspace/test_wordfreq.sh" ]]; then
        OUTCOME_TESTS_EXIST=1

        # Try running the test script
        if OUTCOME_TEST_RESULTS=$(cd "$workspace" && timeout 30 bash test_wordfreq.sh 2>&1); then
            local total_tests pass_count fail_count
            # Count PASS/FAIL lines — use word-boundary matching to avoid
            # false positives from "error handling" descriptions or "passed" in prose.
            # Only count lines where PASS/FAIL/OK appear as the first word or test status.
            total_tests=$(echo "$OUTCOME_TEST_RESULTS" | grep -ciE '^\s*(PASS|FAIL|OK|ERROR)\b' || echo "0")
            pass_count=$(echo "$OUTCOME_TEST_RESULTS" | grep -ciE '^\s*(PASS|OK)\b' || echo "0")
            fail_count=$(echo "$OUTCOME_TEST_RESULTS" | grep -ciE '^\s*(FAIL|ERROR)\b' || echo "0")
            if [[ $total_tests -gt 0 && $fail_count -eq 0 ]]; then
                OUTCOME_TESTS_PASS=1
            fi
        fi
    fi
}

# ── Main test loop ─────────────────────────────────────────────

# Master results directory
MASTER_DIR=$(mktemp -d /tmp/nash_test_suite.XXXXXX)
log_header "Nash Coding Task Test Suite"
log_info "Binary:    $NASH_BIN"
log_info "API:       $API_URL"
log_info "Runs:      $NUM_RUNS"
log_info "Results:   $MASTER_DIR"
log_info "Started:   $(date -Iseconds)"
echo ""

# Check API is reachable
if ! curl -sf --max-time 5 "$API_URL/v1/models" >/dev/null 2>&1; then
    # Try /props endpoint (llama.cpp)
    if ! curl -sf --max-time 5 "$API_URL/props" >/dev/null 2>&1; then
        log_warn "API at $API_URL may not be reachable (continuing anyway)"
    fi
fi

# Arrays to accumulate per-run results for final analysis
declare -a RUN_EXITS=()
declare -a RUN_DURATIONS=()
declare -a RUN_TOTAL_STEPS=()
declare -a RUN_FILE_EXISTS=()
declare -a RUN_COMPILES=()
declare -a RUN_RUNS=()
declare -a RUN_TESTS_EXIST=()
declare -a RUN_TESTS_PASS=()
declare -a RUN_TOTAL_PROMPT_TOKENS=()
declare -a RUN_TOTAL_COMPLETION_TOKENS=()
declare -a RUN_LOOP_DURATIONS=()

for run in $(seq 1 "$NUM_RUNS"); do
    log_header "Run $run of $NUM_RUNS"

    # Create isolated directories for this run
    RUN_DIR="$MASTER_DIR/run_$run"
    DATA_DIR="$RUN_DIR/nash_data"
    WORKSPACE="/tmp/nash_test_workspace"
    mkdir -p "$RUN_DIR" "$DATA_DIR" "$WORKSPACE"

    # Clean workspace from previous runs
    rm -rf "$WORKSPACE"/*

    run_total_duration=0
    run_total_steps=0
    run_total_prompt_tokens=0
    run_total_completion_tokens=0
    run_all_exits=()
    loop_durations=""

    # ── React Loop 0: Initial coding task ──────────────────────
    log_info "Loop 0: Initial coding task..."
    run_nash "$DATA_DIR" "$PROMPT_INITIAL"
    loop0_exit=$NASH_EXIT
    loop0_duration=$NASH_DURATION
    run_all_exits+=($loop0_exit)

    # Save outputs
    echo "$NASH_STDOUT" > "$RUN_DIR/loop0_stdout.txt"
    echo "$NASH_STDERR" > "$RUN_DIR/loop0_stderr.txt"
    echo "$NASH_EXIT" > "$RUN_DIR/loop0_exit.txt"

    SESSION_DIR="$NASH_SESSION_DIR"
    echo "$SESSION_DIR" > "$RUN_DIR/session_dir.txt"

    parse_stderr_metrics "$NASH_STDERR"
    loop0_steps=$METRIC_STEPS
    run_total_steps=$((run_total_steps + loop0_steps))
    run_total_duration=$(echo "$run_total_duration + $loop0_duration" | bc)
    run_total_prompt_tokens=$((run_total_prompt_tokens + METRIC_PROMPT_TOKENS))
    run_total_completion_tokens=$((run_total_completion_tokens + METRIC_COMPLETION_TOKENS))
    loop_durations="${loop0_duration}"

    if [[ $loop0_exit -eq 0 ]]; then
        log_ok "Loop 0 completed (${loop0_steps} steps, ${loop0_duration}s, exit=$loop0_exit)"
    else
        log_error "Loop 0 failed (${loop0_steps} steps, ${loop0_duration}s, exit=$loop0_exit)"
    fi

    # Save per-loop metrics
    cat > "$RUN_DIR/loop0_metrics.json" <<EOF
{
    "loop": 0,
    "exit_code": $loop0_exit,
    "duration_s": $loop0_duration,
    "steps": $loop0_steps,
    "prompt_tokens": $METRIC_PROMPT_TOKENS,
    "completion_tokens": $METRIC_COMPLETION_TOKENS,
    "pp_speed": $METRIC_PP_SPEED,
    "gen_speed": $METRIC_GEN_SPEED,
    "ctx_pct": $METRIC_CTX_PCT,
    "tool_calls": {
        "shell_exec": $METRIC_SHELL_EXEC,
        "file_read": $METRIC_FILE_READ,
        "file_write": $METRIC_FILE_WRITE,
        "file_edit": $METRIC_FILE_EDIT,
        "grep_search": $METRIC_GREP_SEARCH
    },
    "errors": $METRIC_ERRORS,
    "warnings": $METRIC_WARNINGS
}
EOF

    if [[ $VERBOSE -eq 1 ]]; then
        echo "  stdout (first 200 chars): ${NASH_STDOUT:0:200}"
    fi

    # ── React Loop 1: Follow-up (add features) ────────────────
    if [[ -n "$SESSION_DIR" && -d "$SESSION_DIR" ]]; then
        log_info "Loop 1: Adding features (follow-up)..."
        run_nash "$DATA_DIR" "$PROMPT_FOLLOWUP1" "$SESSION_DIR"
        loop1_exit=$NASH_EXIT
        loop1_duration=$NASH_DURATION
        run_all_exits+=($loop1_exit)

        echo "$NASH_STDOUT" > "$RUN_DIR/loop1_stdout.txt"
        echo "$NASH_STDERR" > "$RUN_DIR/loop1_stderr.txt"
        echo "$NASH_EXIT" > "$RUN_DIR/loop1_exit.txt"

        parse_stderr_metrics "$NASH_STDERR"
        loop1_steps=$METRIC_STEPS
        run_total_steps=$((run_total_steps + loop1_steps))
        run_total_duration=$(echo "$run_total_duration + $loop1_duration" | bc)
        run_total_prompt_tokens=$((run_total_prompt_tokens + METRIC_PROMPT_TOKENS))
        run_total_completion_tokens=$((run_total_completion_tokens + METRIC_COMPLETION_TOKENS))
        loop_durations="${loop_durations},${loop1_duration}"

        if [[ $loop1_exit -eq 0 ]]; then
            log_ok "Loop 1 completed (${loop1_steps} steps, ${loop1_duration}s, exit=$loop1_exit)"
        else
            log_error "Loop 1 failed (${loop1_steps} steps, ${loop1_duration}s, exit=$loop1_exit)"
        fi

        cat > "$RUN_DIR/loop1_metrics.json" <<EOF
{
    "loop": 1,
    "exit_code": $loop1_exit,
    "duration_s": $loop1_duration,
    "steps": $loop1_steps,
    "prompt_tokens": $METRIC_PROMPT_TOKENS,
    "completion_tokens": $METRIC_COMPLETION_TOKENS,
    "pp_speed": $METRIC_PP_SPEED,
    "gen_speed": $METRIC_GEN_SPEED,
    "ctx_pct": $METRIC_CTX_PCT,
    "tool_calls": {
        "shell_exec": $METRIC_SHELL_EXEC,
        "file_read": $METRIC_FILE_READ,
        "file_write": $METRIC_FILE_WRITE,
        "file_edit": $METRIC_FILE_EDIT,
        "grep_search": $METRIC_GREP_SEARCH
    },
    "errors": $METRIC_ERRORS,
    "warnings": $METRIC_WARNINGS
}
EOF
    else
        log_warn "No session directory found — skipping follow-up loops"
        loop1_exit=1
        loop1_duration=0
        loop1_steps=0
        run_all_exits+=(1)
    fi

    # ── React Loop 2: Follow-up (bug fixes + tests) ───────────
    if [[ -n "$SESSION_DIR" && -d "$SESSION_DIR" ]]; then
        log_info "Loop 2: Bug fixes and testing (follow-up)..."
        run_nash "$DATA_DIR" "$PROMPT_FOLLOWUP2" "$SESSION_DIR"
        loop2_exit=$NASH_EXIT
        loop2_duration=$NASH_DURATION
        run_all_exits+=($loop2_exit)

        echo "$NASH_STDOUT" > "$RUN_DIR/loop2_stdout.txt"
        echo "$NASH_STDERR" > "$RUN_DIR/loop2_stderr.txt"
        echo "$NASH_EXIT" > "$RUN_DIR/loop2_exit.txt"

        parse_stderr_metrics "$NASH_STDERR"
        loop2_steps=$METRIC_STEPS
        run_total_steps=$((run_total_steps + loop2_steps))
        run_total_duration=$(echo "$run_total_duration + $loop2_duration" | bc)
        run_total_prompt_tokens=$((run_total_prompt_tokens + METRIC_PROMPT_TOKENS))
        run_total_completion_tokens=$((run_total_completion_tokens + METRIC_COMPLETION_TOKENS))
        loop_durations="${loop_durations},${loop2_duration}"

        if [[ $loop2_exit -eq 0 ]]; then
            log_ok "Loop 2 completed (${loop2_steps} steps, ${loop2_duration}s, exit=$loop2_exit)"
        else
            log_error "Loop 2 failed (${loop2_steps} steps, ${loop2_duration}s, exit=$loop2_exit)"
        fi

        cat > "$RUN_DIR/loop2_metrics.json" <<EOF
{
    "loop": 2,
    "exit_code": $loop2_exit,
    "duration_s": $loop2_duration,
    "steps": $loop2_steps,
    "prompt_tokens": $METRIC_PROMPT_TOKENS,
    "completion_tokens": $METRIC_COMPLETION_TOKENS,
    "pp_speed": $METRIC_PP_SPEED,
    "gen_speed": $METRIC_GEN_SPEED,
    "ctx_pct": $METRIC_CTX_PCT,
    "tool_calls": {
        "shell_exec": $METRIC_SHELL_EXEC,
        "file_read": $METRIC_FILE_READ,
        "file_write": $METRIC_FILE_WRITE,
        "file_edit": $METRIC_FILE_EDIT,
        "grep_search": $METRIC_GREP_SEARCH
    },
    "errors": $METRIC_ERRORS,
    "warnings": $METRIC_WARNINGS
}
EOF
    else
        loop2_exit=1
        loop2_duration=0
        loop2_steps=0
        run_all_exits+=(1)
    fi

    # ── Check outcomes after all 3 loops ───────────────────────
    check_outcomes "$WORKSPACE"

    # Determine overall run success
    run_exit=0
    for e in "${run_all_exits[@]}"; do
        if [[ $e -ne 0 ]]; then run_exit=1; fi
    done

    # Analyze journal
    JOURNAL_PATH=""
    if [[ -n "$SESSION_DIR" && -f "$SESSION_DIR/journal.jsonl" ]]; then
        JOURNAL_PATH="$SESSION_DIR/journal.jsonl"
        cp "$JOURNAL_PATH" "$RUN_DIR/journal.jsonl"
        parse_journal "$JOURNAL_PATH"

        # Save journal analysis
        cat > "$RUN_DIR/journal_analysis.json" <<EOF
{
    "total_journal_steps": $JOURNAL_STEPS,
    "react_loops_in_journal": $JOURNAL_REACT_LOOPS,
    "journal_errors": $JOURNAL_ERRORS
}
EOF
    fi

    # Save run summary
    cat > "$RUN_DIR/summary.json" <<EOF
{
    "run": $run,
    "total_duration_s": $run_total_duration,
    "total_steps": $run_total_steps,
    "total_prompt_tokens": $run_total_prompt_tokens,
    "total_completion_tokens": $run_total_completion_tokens,
    "loop_exits": [${run_all_exits[0]:-1}, ${run_all_exits[1]:-1}, ${run_all_exits[2]:-1}],
    "overall_exit": $run_exit,
    "outcomes": {
        "file_exists": $OUTCOME_FILE_EXISTS,
        "compiles": $OUTCOME_COMPILES,
        "runs": $OUTCOME_RUNS,
        "tests_exist": $OUTCOME_TESTS_EXIST,
        "tests_pass": $OUTCOME_TESTS_PASS
    }
}
EOF

    # Copy workspace artifacts
    if [[ -d "$WORKSPACE" ]]; then
        cp -r "$WORKSPACE" "$RUN_DIR/workspace" 2>/dev/null || true
    fi

    # Accumulate for final analysis
    RUN_EXITS+=($run_exit)
    RUN_DURATIONS+=("$run_total_duration")
    RUN_TOTAL_STEPS+=($run_total_steps)
    RUN_FILE_EXISTS+=($OUTCOME_FILE_EXISTS)
    RUN_COMPILES+=($OUTCOME_COMPILES)
    RUN_RUNS+=($OUTCOME_RUNS)
    RUN_TESTS_EXIST+=($OUTCOME_TESTS_EXIST)
    RUN_TESTS_PASS+=($OUTCOME_TESTS_PASS)
    RUN_TOTAL_PROMPT_TOKENS+=($run_total_prompt_tokens)
    RUN_TOTAL_COMPLETION_TOKENS+=($run_total_completion_tokens)
    RUN_LOOP_DURATIONS+=("$loop_durations")

    # Print run summary
    echo ""
    echo "  ┌─── Run $run Summary ───────────────────────────────"
    echo "  │ Duration:    ${run_total_duration}s total"
    echo "  │ Steps:       $run_total_steps total (L0:${loop0_steps} L1:${loop1_steps:-0} L2:${loop2_steps:-0})"
    echo "  │ Tokens:      ${run_total_prompt_tokens} prompt + ${run_total_completion_tokens} completion"
    echo "  │ Exits:       L0:${run_all_exits[0]} L1:${run_all_exits[1]:-?} L2:${run_all_exits[2]:-?}"
    echo "  │ File exists: $([ $OUTCOME_FILE_EXISTS -eq 1 ] && echo '✓' || echo '✗')"
    echo "  │ Compiles:    $([ $OUTCOME_COMPILES -eq 1 ] && echo '✓' || echo '✗')"
    echo "  │ Runs:        $([ $OUTCOME_RUNS -eq 1 ] && echo '✓' || echo '✗')"
    echo "  │ Tests exist: $([ $OUTCOME_TESTS_EXIST -eq 1 ] && echo '✓' || echo '✗')"
    echo "  │ Tests pass:  $([ $OUTCOME_TESTS_PASS -eq 1 ] && echo '✓' || echo '✗')"
    echo "  └────────────────────────────────────────────────────"

done

# ── Final Analysis ─────────────────────────────────────────────
log_header "Final Analysis ($NUM_RUNS runs)"

# Helper: compute stats from an array of numbers
compute_stats() {
    local -n arr=$1
    local sum=0 min=999999 max=0 count=${#arr[@]}

    if [[ $count -eq 0 ]]; then
        echo "0 0 0 0"
        return
    fi

    for v in "${arr[@]}"; do
        sum=$(echo "$sum + $v" | bc)
        if (( $(echo "$v < $min" | bc -l) )); then min=$v; fi
        if (( $(echo "$v > $max" | bc -l) )); then max=$v; fi
    done

    local avg
    avg=$(echo "scale=2; $sum / $count" | bc)
    echo "$avg $min $max $sum"
}

# Count successes
count_ones() {
    local -n arr=$1
    local c=0
    for v in "${arr[@]}"; do
        if [[ $v -eq 1 ]]; then ((c++)) || true; fi
    done
    echo $c
}

success_count=$(count_ones RUN_FILE_EXISTS)
compile_count=$(count_ones RUN_COMPILES)
runs_count=$(count_ones RUN_RUNS)
tests_exist_count=$(count_ones RUN_TESTS_EXIST)
tests_pass_count=$(count_ones RUN_TESTS_PASS)
exit_success=0
for e in "${RUN_EXITS[@]}"; do
    if [[ $e -eq 0 ]]; then ((exit_success++)) || true; fi
done

read -r dur_avg dur_min dur_max dur_sum <<< "$(compute_stats RUN_DURATIONS)"
read -r step_avg step_min step_max step_sum <<< "$(compute_stats RUN_TOTAL_STEPS)"
read -r pt_avg pt_min pt_max pt_sum <<< "$(compute_stats RUN_TOTAL_PROMPT_TOKENS)"
read -r ct_avg ct_min ct_max ct_sum <<< "$(compute_stats RUN_TOTAL_COMPLETION_TOKENS)"

echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│                    AGGREGATE RESULTS                        │"
echo "├─────────────────────────────────────────────────────────────┤"
printf "│ %-25s %10s %10s %10s │\n" "Metric" "Rate" "Count" "Total"
echo "├─────────────────────────────────────────────────────────────┤"
printf "│ %-25s %9s%% %10s %10s │\n" "All loops succeed" \
    "$(echo "scale=0; $exit_success * 100 / $NUM_RUNS" | bc)" \
    "$exit_success" "$NUM_RUNS"
printf "│ %-25s %9s%% %10s %10s │\n" "File created" \
    "$(echo "scale=0; $success_count * 100 / $NUM_RUNS" | bc)" \
    "$success_count" "$NUM_RUNS"
printf "│ %-25s %9s%% %10s %10s │\n" "Compiles" \
    "$(echo "scale=0; $compile_count * 100 / $NUM_RUNS" | bc)" \
    "$compile_count" "$NUM_RUNS"
printf "│ %-25s %9s%% %10s %10s │\n" "Runs correctly" \
    "$(echo "scale=0; $runs_count * 100 / $NUM_RUNS" | bc)" \
    "$runs_count" "$NUM_RUNS"
printf "│ %-25s %9s%% %10s %10s │\n" "Test script created" \
    "$(echo "scale=0; $tests_exist_count * 100 / $NUM_RUNS" | bc)" \
    "$tests_exist_count" "$NUM_RUNS"
printf "│ %-25s %9s%% %10s %10s │\n" "All tests pass" \
    "$(echo "scale=0; $tests_pass_count * 100 / $NUM_RUNS" | bc)" \
    "$tests_pass_count" "$NUM_RUNS"
echo "├─────────────────────────────────────────────────────────────┤"
printf "│ %-25s %10s %10s %10s │\n" "Metric" "Avg" "Min" "Max"
echo "├─────────────────────────────────────────────────────────────┤"
printf "│ %-25s %9ss %9ss %9ss │\n" "Duration (3 loops)" "$dur_avg" "$dur_min" "$dur_max"
printf "│ %-25s %10s %10s %10s │\n" "Steps (3 loops)" "$step_avg" "$step_min" "$step_max"
printf "│ %-25s %10s %10s %10s │\n" "Prompt tokens" "$pt_avg" "$pt_min" "$pt_max"
printf "│ %-25s %10s %10s %10s │\n" "Completion tokens" "$ct_avg" "$ct_min" "$ct_max"
echo "└─────────────────────────────────────────────────────────────┘"
echo ""

# ── Per-loop timing breakdown ──────────────────────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│                  PER-LOOP TIMING BREAKDOWN                  │"
echo "├─────────────────────────────────────────────────────────────┤"
printf "│ %-6s │ %-14s │ %-14s │ %-14s │\n" "Run" "Loop 0 (s)" "Loop 1 (s)" "Loop 2 (s)"
echo "├─────────────────────────────────────────────────────────────┤"
for i in $(seq 0 $((NUM_RUNS - 1))); do
    IFS=',' read -r l0 l1 l2 <<< "${RUN_LOOP_DURATIONS[$i]}"
    printf "│ %-6s │ %14s │ %14s │ %14s │\n" \
        "$((i + 1))" "${l0:-n/a}" "${l1:-n/a}" "${l2:-n/a}"
done
echo "└─────────────────────────────────────────────────────────────┘"
echo ""

# ── Tool usage analysis across all runs ────────────────────────
log_info "Tool usage analysis (from journals):"
echo ""
all_journals=$(find "$MASTER_DIR" -name 'journal.jsonl' -type f 2>/dev/null)
if [[ -n "$all_journals" ]]; then
    echo "  Tool call distribution across all runs:"
    echo "$all_journals" | xargs cat | jq -r '.tool // empty' | sort | uniq -c | sort -rn | while read -r count tool; do
        printf "    %-20s %5d calls\n" "$tool" "$count"
    done
    echo ""

    echo "  Failed tool calls across all runs:"
    failed_count=$(echo "$all_journals" | xargs cat | jq -r 'select(.failed == true) | .tool' | wc -l)
    if [[ $failed_count -gt 0 ]]; then
        echo "$all_journals" | xargs cat | jq -r 'select(.failed == true) | .tool' | sort | uniq -c | sort -rn | while read -r count tool; do
            printf "    %-20s %5d failures\n" "$tool" "$count"
        done
    else
        echo "    (none)"
    fi
    echo ""

    echo "  React loop counts per journal:"
    for jf in $all_journals; do
        local_run=$(basename "$(dirname "$jf")")
        loops=$(jq -r '.react_loop' "$jf" | sort -nu | tr '\n' ',' | sed 's/,$//')
        max_step=$(jq -r '.step' "$jf" | sort -n | tail -1)
        echo "    $local_run: loops=[$loops] max_step=$max_step"
    done
fi

echo ""

# ── Detailed error analysis ───────────────────────────────────
error_files=$(find "$MASTER_DIR" -name 'loop*_stderr.txt' -type f | sort)
total_errors=0
if [[ -n "$error_files" ]]; then
    for ef in $error_files; do
        errs=$(grep -c '^\[error\]' "$ef" 2>/dev/null || true)
        total_errors=$((total_errors + errs))
    done
fi
log_info "Total errors across all loops: $total_errors"
if [[ $total_errors -gt 0 ]]; then
    for ef in $error_files; do
        errs=$(grep '^\[error\]' "$ef" 2>/dev/null || true)
        if [[ -n "$errs" ]]; then
            local_run=$(echo "$ef" | grep -oP 'run_\d+')
            local_loop=$(echo "$ef" | grep -oP 'loop\d+')
            echo "  $local_run/$local_loop:"
            echo "$errs" | sed 's/^/    /'
        fi
    done
fi

# ── Write final report ────────────────────────────────────────
REPORT_FILE="$MASTER_DIR/report.json"
cat > "$REPORT_FILE" <<EOF
{
    "test_suite": "nash_coding_react",
    "timestamp": "$(date -Iseconds)",
    "api_url": "$API_URL",
    "nash_binary": "$NASH_BIN",
    "num_runs": $NUM_RUNS,
    "aggregate": {
        "success_rate": {
            "all_loops_succeed": "$exit_success/$NUM_RUNS",
            "file_created": "$success_count/$NUM_RUNS",
            "compiles": "$compile_count/$NUM_RUNS",
            "runs_correctly": "$runs_count/$NUM_RUNS",
            "tests_created": "$tests_exist_count/$NUM_RUNS",
            "tests_pass": "$tests_pass_count/$NUM_RUNS"
        },
        "duration_s": { "avg": $dur_avg, "min": $dur_min, "max": $dur_max, "total": $dur_sum },
        "steps":      { "avg": $step_avg, "min": $step_min, "max": $step_max, "total": $step_sum },
        "prompt_tokens":     { "avg": $pt_avg, "min": $pt_min, "max": $pt_max, "total": $pt_sum },
        "completion_tokens": { "avg": $ct_avg, "min": $ct_min, "max": $ct_max, "total": $ct_sum },
        "total_errors": $total_errors
    }
}
EOF

echo ""
log_ok "Report written to: $REPORT_FILE"
log_ok "Full results in:   $MASTER_DIR"
log_info "Finished:  $(date -Iseconds)"
echo ""

# ── Exit with overall status ──────────────────────────────────
if [[ $exit_success -eq $NUM_RUNS ]]; then
    log_ok "ALL $NUM_RUNS RUNS SUCCEEDED"
    exit 0
elif [[ $exit_success -gt 0 ]]; then
    log_warn "$exit_success/$NUM_RUNS runs succeeded"
    exit 0
else
    log_error "ALL $NUM_RUNS RUNS FAILED"
    exit 1
fi
