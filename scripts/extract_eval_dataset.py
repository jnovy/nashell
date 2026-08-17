#!/usr/bin/env python3
"""Extract retrospective evaluation dataset from nash session journals.

Scans all session journals for sessions with both memory_context and
memory_quality events, producing a JSONL file with one row per session
containing: query, all scored candidates, recalled keys, task outcome.
"""

import json
import os
import sys

SESSIONS_DIR = os.path.expanduser("~/.nash/sessions")
OUTPUT = os.path.join(os.path.dirname(__file__), "eval_dataset.jsonl")


def extract_session(session_dir):
    """Extract memory_context and memory_quality from a session journal.
    
    Returns dict with session data or None if incomplete.
    """
    jpath = os.path.join(session_dir, "journal.jsonl")
    if not os.path.exists(jpath):
        return None

    context_event = None
    quality_event = None

    with open(jpath, "rb") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                line_str = line.decode("utf-8", errors="replace")
                event = json.loads(line_str)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue

            tool = event.get("tool", "")
            if tool == "memory_context" and context_event is None:
                context_event = event
            elif tool == "memory_quality" and quality_event is None:
                quality_event = event

            if context_event and quality_event:
                break

    if not context_event or not quality_event:
        return None

    ctx = context_event.get("params", {})
    qual = quality_event.get("params", {})

    # Merge all candidate lists into one with type annotation
    candidates = []
    for cat in ["skills_matched", "lessons_matched",
                "strategies_matched", "antipatterns_matched"]:
        for entry in ctx.get(cat, []):
            entry_copy = dict(entry)
            entry_copy["category"] = cat.replace("_matched", "")
            candidates.append(entry_copy)

    session_id = os.path.basename(session_dir)

    return {
        "session_id": session_id,
        "query": ctx.get("query", ""),
        "pinned_keys": ctx.get("pinned_keys", []),
        "candidates": candidates,
        "recalled_keys": qual.get("recalled_keys", []),
        "n_recalled": qual.get("n_recalled", 0),
        "task_succeeded": qual.get("task_succeeded", False),
        "cold_start_count": qual.get("cold_start_count", 0),
        "cold_start_pct": qual.get("cold_start_pct", 0),
    }


def main():
    output_path = OUTPUT
    if len(sys.argv) > 1:
        output_path = sys.argv[1]

    dirs = sorted(
        d for d in os.listdir(SESSIONS_DIR)
        if d[0].isdigit()
    )

    rows = []
    for d in dirs:
        session_dir = os.path.join(SESSIONS_DIR, d)
        row = extract_session(session_dir)
        if row:
            rows.append(row)

    with open(output_path, "w") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")

    # Summary
    n_success = sum(1 for r in rows if r["task_succeeded"])
    n_fail = sum(1 for r in rows if not r["task_succeeded"])
    total_candidates = sum(len(r["candidates"]) for r in rows)
    total_recalled = sum(r["n_recalled"] for r in rows)
    avg_candidates = total_candidates / len(rows) if rows else 0
    avg_recalled = total_recalled / len(rows) if rows else 0

    print(f"Extracted {len(rows)} sessions to {output_path}")
    print(f"  Succeeded: {n_success} ({100*n_success/len(rows):.1f}%)")
    print(f"  Failed:    {n_fail} ({100*n_fail/len(rows):.1f}%)")
    print(f"  Avg candidates/session: {avg_candidates:.1f}")
    print(f"  Avg recalled/session:   {avg_recalled:.1f}")


if __name__ == "__main__":
    main()
