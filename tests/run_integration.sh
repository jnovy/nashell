#!/bin/bash
# Nash integration tests — runs against a real LLM server
# ALWAYS uses --data-dir with a unique temp directory (NEVER production ~/.nash/)
#
# Usage: ./tests/run_integration.sh [--api URL]
# Default API: http://192.168.1.18:8080

set -e

API_BASE="${1:-http://192.168.1.18:8080}"
NASH="./nash"
TEST_DIR=$(mktemp -d /tmp/nash-integration-test-XXXXXX)
PASS=0
FAIL=0

echo "=== Nash Integration Tests ==="
echo "API:      $API_BASE"
echo "Data dir: $TEST_DIR"
echo "Binary:   $NASH"
echo ""

# Check prerequisites
if [ ! -x "$NASH" ]; then
    echo "ERROR: nash binary not found. Run 'make' first."
    exit 1
fi

# Check server is accessible
if ! curl -s -m 5 "$API_BASE/v1/models" >/dev/null 2>&1; then
    echo "ERROR: LLM server not accessible at $API_BASE"
    exit 1
fi

run_test() {
    local name="$1"
    local query="$2"
    local expected="$3"
    
    printf "  %-50s" "$name"
    
    local result
    result=$($NASH --api "$API_BASE" --data-dir "$TEST_DIR" -p "$query" 2>/dev/null | tail -1)
    
    if echo "$result" | grep -qi "$expected" >/dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (expected '$expected', got '$result')"
        FAIL=$((FAIL + 1))
    fi
}

run_test_exists() {
    local name="$1"
    local query="$2"
    local check_path="$3"
    
    printf "  %-50s" "$name"
    
    $NASH --api "$API_BASE" --data-dir "$TEST_DIR" -p "$query" >/dev/null 2>&1
    
    if [ -e "$check_path" ]; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (expected $check_path to exist)"
        FAIL=$((FAIL + 1))
    fi
}

echo "--- Basic ---"
run_test "Simple arithmetic" "what is 2+2?" "4"
run_test "Simple text" "say hello in one word" "hello"

echo ""
echo "--- Multi-step ---"
run_test "List files" "list all .c files in src/ directory" "main.c"
run_test "Kernel version" "what kernel are we running? answer with just the version" "fc4"

echo ""
echo "--- Memory ---"
# Store a memory
$NASH --api "$API_BASE" --data-dir "$TEST_DIR" -p \
    'use memory_store to save key "fact:test-value" with value "the secret number is 42" and tags "test"' \
    >/dev/null 2>&1

# Check memory file exists
printf "  %-50s" "Memory store creates file"
if ls "$TEST_DIR/memory/"*.json >/dev/null 2>&1; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

# Recall the memory
run_test "Memory recall" "use memory_recall to search for 'secret number'" "42"

echo ""
echo "--- Store ---"
printf "  %-50s" "Store directory created"
if [ -d "$TEST_DIR/store" ]; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

printf "  %-50s" "Sessions directory created"
if [ -d "$TEST_DIR/sessions" ]; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

printf "  %-50s" "Store has content-addressed files"
store_count=$(ls "$TEST_DIR/store/" 2>/dev/null | wc -l)
if [ "$store_count" -gt 0 ]; then
    echo "PASS ($store_count blobs)"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

printf "  %-50s" "Session has symlinks (R0S0, etc.)"
symlink_count=$(find "$TEST_DIR/sessions/" -type l 2>/dev/null | wc -l)
if [ "$symlink_count" -gt 0 ]; then
    echo "PASS ($symlink_count symlinks)"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

# Cleanup
rm -rf "$TEST_DIR"
echo "Cleaned up: $TEST_DIR"

exit $FAIL
