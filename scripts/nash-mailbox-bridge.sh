#!/bin/bash
# nash-mailbox-bridge.sh — interact with nash's file-based mailbox
#
# All messages are plain text. No JSON. No dependencies beyond bash.
#
# Usage:
#   nash-mailbox-bridge.sh submit "your query here"
#   nash-mailbox-bridge.sh watch
#   nash-mailbox-bridge.sh answer ID "your answer"
#   nash-mailbox-bridge.sh ls
#   nash-mailbox-bridge.sh status

set -euo pipefail

MAILBOX_DIR="${NASH_MAILBOX_DIR:-${HOME}/.nash/mailbox}"
INBOX="${MAILBOX_DIR}/inbox"
OUTBOX="${MAILBOX_DIR}/outbox"

# Ensure directories exist
mkdir -p "${INBOX}" "${OUTBOX}"

usage() {
    cat <<EOF
nash-mailbox-bridge.sh — interact with nash's file-based mailbox

All messages are plain text files. No JSON required.

Usage:
  $0 submit QUERY        Submit a task to nash daemon
  $0 watch               Watch outbox for questions/status/results
  $0 answer ID TEXT      Answer a user_ask question
  $0 ls                  List pending messages
  $0 status              Show mailbox status

Examples:
  # Start nash daemon in one terminal:
  nash --daemon

  # Submit a task:
  $0 submit "List all files in /tmp"

  # Or just write it directly:
  echo "List all files in /tmp" > ~/.nash/mailbox/inbox/task_myquery

  # Watch for questions:
  $0 watch

  # Answer a question:
  $0 answer 6849a12f0042 "Yes, proceed with deletion"

  # Or just write it directly:
  echo "Yes, proceed" > ~/.nash/mailbox/inbox/ask_6849a12f0042

Environment:
  NASH_MAILBOX_DIR   Override mailbox directory (default: ~/.nash/mailbox)
EOF
    exit 1
}

cmd_submit() {
    local query="$1"
    local id
    id=$(printf '%x%04x' "$(date +%s)" "$((RANDOM % 10000))")
    local task_file="${INBOX}/task_${id}"
    local tmp="${task_file}.tmp"

    printf '%s\n' "${query}" > "${tmp}"
    mv "${tmp}" "${task_file}"

    echo "[submit] task ${id} submitted"
    echo "[submit] query: ${query}"
    echo "[submit] result will appear in: ${OUTBOX}/result_${id}"
}

cmd_answer() {
    local id="$1"
    local answer="$2"
    local answer_file="${INBOX}/ask_${id}"
    local tmp="${answer_file}.tmp"

    printf '%s\n' "${answer}" > "${tmp}"
    mv "${tmp}" "${answer_file}"

    echo "[answer] answer written to ${answer_file}"
}

cmd_watch() {
    echo "[watch] Watching ${OUTBOX} for messages..."
    echo "[watch] Press Ctrl+C to stop"
    echo

    # Use inotifywait if available, fall back to polling
    if command -v inotifywait &>/dev/null; then
        inotifywait -m -e create -e moved_to "${OUTBOX}" 2>/dev/null | while read -r _ _ file; do
            # Skip .tmp files
            [[ "${file}" == *.tmp ]] && continue
            local fpath="${OUTBOX}/${file}"
            if [ -f "${fpath}" ]; then
                echo "━━━ ${file} ━━━"
                cat "${fpath}"
                echo

                # If it's a user_ask, show how to answer
                if [[ "${file}" == ask_* ]]; then
                    local ask_id="${file#ask_}"
                    echo ">>> To answer: $0 answer ${ask_id} \"your answer\""
                    echo ">>> Or:        echo \"your answer\" > ${INBOX}/ask_${ask_id}"
                    echo
                fi
            fi
        done
    else
        echo "[watch] inotifywait not found, polling (1s interval)"
        local seen=""
        while true; do
            for f in "${OUTBOX}"/*; do
                [ -f "$f" ] || continue
                [[ "$f" == *.tmp ]] && continue
                local base
                base=$(basename "$f")
                if [[ "${seen}" != *"${base}"* ]]; then
                    echo "━━━ ${base} ━━━"
                    cat "$f"
                    echo

                    if [[ "${base}" == ask_* ]]; then
                        local ask_id="${base#ask_}"
                        echo ">>> To answer: $0 answer ${ask_id} \"your answer\""
                        echo ">>> Or:        echo \"your answer\" > ${INBOX}/ask_${ask_id}"
                        echo
                    fi
                    seen="${seen} ${base}"
                fi
            done
            sleep 1
        done
    fi
}

cmd_ls() {
    echo "=== Outbox ==="
    local found=0
    for f in "${OUTBOX}"/*; do
        [ -f "$f" ] || continue
        [[ "$f" == *.tmp ]] && continue
        local base
        base=$(basename "$f")
        # Show type and first line as preview
        local preview
        preview=$(head -1 "$f" 2>/dev/null | cut -c1-80)
        echo "  ${base}  │ ${preview}"
        found=1
    done
    [ $found -eq 0 ] && echo "  (empty)"
    echo
    echo "=== Inbox ==="
    found=0
    for f in "${INBOX}"/*; do
        [ -f "$f" ] || continue
        [[ "$f" == *.tmp ]] && continue
        local base
        base=$(basename "$f")
        local preview
        preview=$(head -1 "$f" 2>/dev/null | cut -c1-80)
        echo "  ${base}  │ ${preview}"
        found=1
    done
    [ $found -eq 0 ] && echo "  (empty)"
}

cmd_status() {
    echo "Mailbox: ${MAILBOX_DIR}"
    echo "Inbox:   ${INBOX}"
    echo "Outbox:  ${OUTBOX}"
    echo
    local inbox_count outbox_count
    inbox_count=$(find "${INBOX}" -maxdepth 1 -type f ! -name '*.tmp' 2>/dev/null | wc -l)
    outbox_count=$(find "${OUTBOX}" -maxdepth 1 -type f ! -name '*.tmp' 2>/dev/null | wc -l)
    echo "Inbox messages:  ${inbox_count}"
    echo "Outbox messages: ${outbox_count}"
}

# Main dispatch
case "${1:-}" in
    submit)
        [ -n "${2:-}" ] || { echo "Error: query required"; usage; }
        cmd_submit "$2"
        ;;
    answer)
        [ -n "${2:-}" ] && [ -n "${3:-}" ] || { echo "Error: ID and answer required"; usage; }
        cmd_answer "$2" "$3"
        ;;
    watch)
        cmd_watch
        ;;
    ls)
        cmd_ls
        ;;
    status)
        cmd_status
        ;;
    *)
        usage
        ;;
esac
