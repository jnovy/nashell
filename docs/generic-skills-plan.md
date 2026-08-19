# Generic Skills Layer - Implementation and Testing Plan

## Background

Paper "Do Personalized Skills Help Coding Agents?" (arXiv 2608.10319) found
that generic skills pooled across developers (+3.78%) beat personalized skills
(+0.97%) when individual history is sparse. Nash's two-layer memory
architecture (workspace-local + global) already provides the physical
separation needed, but lacks sparsity-aware boosting and a curated baseline.

This plan implements the high-value changes in three phases, with tests for
each.

---

## Phase 1: Sparsity-Aware Global Skill Boosting (C code)

### Goal

When a workspace has few relevant local skills, automatically boost injection
of global-scope skills to fill the gap.

### 1.1 Add `is_global` flag to `memory_entry_t`

**File:** `src/memory.h` (around line 130-195 in the `memory_entry_t` struct)

Add field:

```c
int is_global;  /* 1 if entry came from global layer, 0 if workspace */
```

**Rationale:** After `workspace_recall()` merges results from both layers, the
origin info is lost. Downstream code (`inject_memory_type()`) needs to know
which layer each result came from to implement sparsity-aware slot filling.

**Why in `memory_entry_t` and not `mem_index_entry_t`:** The index entry lives
in a single layer - its scope is implicit from which `memory_t` it belongs to.
Only the query result struct needs the flag because it crosses layer boundaries
during merge.

### 1.2 Tag entries in `workspace_recall()`

**File:** `src/workspace.c`, function `workspace_recall()` (line 141-230)

After the workspace query (line 152) and global query (line 160), before
merging, tag entries:

```c
/* After line 152: */
for (int i = 0; i < ws_results.count; i++)
    ws_results.entries[i].is_global = 0;

/* After line 160: */
for (int i = 0; i < gl_results.count; i++)
    gl_results.entries[i].is_global = 1;
```

Also ensure `memory_query()` initializes `is_global = 0` by default (in the
result-building loop around memory.c:1375-1427).

### 1.3 Sparsity-aware injection in `inject_memory_type()`

**File:** `src/react_context.c`, function `inject_memory_type()` (line 48-127)

Add a two-pass approach for skill injection:

```
Pass 1: Count workspace-local skills available (is_global==0, prefix matches)
Pass 2: If workspace skills < max_count, inject workspace skills first,
         then fill remaining slots from global skills
```

Alternatively (simpler), add a new wrapper around the existing
`inject_memory_type()` call at line 474-476 that:

1. Counts workspace skills in `all_memories` (loop entries, check
   `prefix=="skill:"` and `is_global==0`)
2. If count < `max_skills`, computes `boost_slots = max_skills - count`
3. Temporarily increases `max_skills` by `boost_slots` (capped at 2x original)
4. Calls `inject_memory_type()` with the boosted count

This is ~20 lines of C inserted before line 474 in `react_context.c`.

### 1.4 Config option: `generic_skill_boost`

**Files:** `src/config.h` (line ~217 area), `src/config.c`

Add:

```c
/* config.h, in nash_config_t: */
int generic_skill_boost;  /* enable sparsity-aware global skill boosting (default 1) */
```

```c
/* config.c, in config_apply_defaults(): */
if (cfg->generic_skill_boost < 0) cfg->generic_skill_boost = 1;

/* config.c, in config_parse [memory] section: */
cfg->generic_skill_boost = toml_int(mem, "generic_skill_boost", 1);
```

### 1.5 Tests for Phase 1

**New file:** `tests/test_workspace.c`

**Add to Makefile:** Append `tests/test_workspace` to `TEST_BIN` (line 158).

Test cases:

```
test_workspace_recall_tags_global()
  - Create workspace_t with two memory_t layers (ws dir + global dir)
  - Store "skill:local-thing" in workspace layer
  - Store "skill:global-thing" in global layer
  - Call workspace_recall() with a query matching both
  - Assert ws entry has is_global==0, global entry has is_global==1

test_workspace_recall_dedup_preserves_flag()
  - Store same key in both layers
  - Call workspace_recall()
  - Assert deduplicated result preserves is_global from the winning (higher-scored) entry

test_sparsity_boost_count()
  - Create workspace with 0 local skills, 5 global skills
  - Simulate the boost logic: verify boosted count == max_skills * 2
  - Create workspace with max_skills local skills, 5 global skills
  - Verify no boost applied (boosted count == max_skills)

test_sparsity_boost_partial()
  - Create workspace with 1 local skill (max_skills=3), 5 global skills
  - Verify boosted count == max_skills + (max_skills - 1) = 5, capped at 2*max_skills=6
```

**Existing file:** `tests/test_memory.c`

Add:

```
test_memory_query_is_global_default()
  - memory_query() on a single memory_t
  - Assert all results have is_global==0 (default)
```

**Build/run:**

```sh
make tests/test_workspace && ./tests/test_workspace
make tests/test_memory && ./tests/test_memory
```

---

## Phase 2: Dream Playbook Promotion Pass

### Goal

During dream consolidation, identify workspace-local skills that should be
promoted to global scope based on cross-workspace evidence.

### 2.1 Add Pass 5 to `playbooks/dream.yaml`

**File:** `playbooks/dream.yaml` (after existing pass 4, before the final
summary/commit)

New pass:

```yaml
  - label: "Promote to global"
    required_tools: [shell_exec, file_read]
    on_error: continue
    prompt: |
      You are performing SKILL PROMOTION for the memory store at: {{memory_dir}}

      Read the scratchpad for context from previous passes.

      TASK: Identify workspace-local skills that should be promoted to global
      scope. A skill is a promotion candidate if:
      1. It has recall_hits >= 3 (proven useful)
      2. Its validation score > 0.65 (reliable)
      3. It is NOT project-specific (does not reference specific file paths,
         project names, or narrow tooling)

      For each candidate:
      1. Read the full entry
      2. Evaluate whether the skill is genuinely generic (applicable across
         projects)
      3. If yes, call memory_store with global=true to create a global copy
      4. The workspace copy remains (workspace_store routes updates to
         existing layer, so future recalls hit the global version)

      CONSTRAINTS:
      - Be conservative - only promote truly generic skills
      - Never promote entries that reference specific repos, file paths, or
        project-specific APIs
      - Maximum 5 promotions per dream cycle
      - Never touch pinned memories
      - Log all decisions (promoted and rejected with reason) to scratchpad

      Call done with a list of promoted skills and reasons.
```

**Note:** Move the git commit from pass 4 to a new final pass 6, or use the
existing `post.commit` mechanism. The commit message in pass 4's prompt needs
updating to not be the final pass.

### 2.2 Add `nash memory promote` CLI command

**File:** `src/commands.c` (or new `src/cmd_memory.c`)

Implement a command that:

1. Reads the entry from workspace memory via `workspace_find_memory()`
2. If found in workspace layer, copies it to global layer via
   `memory_store(ws->global, ...)`
3. Deletes from workspace layer via `memory_delete(ws->workspace, ...)`
4. Preserves recall_hits/misses/access_count

**File:** `src/workspace.h` / `src/workspace.c`

Add:

```c
int workspace_promote(workspace_t *ws, const char *key);
```

Implementation:

```c
int workspace_promote(workspace_t *ws, const char *key) {
    if (!ws || !ws->workspace || !ws->global) return -1;
    mem_index_entry_t *e = memory_find(ws->workspace, key);
    if (!e) return -1;  /* not in workspace layer */
    /* Store in global with all metadata */
    int rc = memory_store(ws->global, e->key, e->value, e->pinned,
                          NULL, /* journal_ref */
                          (const char **)e->refs, e->n_refs,
                          (const char **)e->triggers, e->n_triggers);
    if (rc != 0) { memory_find_free(e); return -1; }
    /* Carry forward validation scores */
    memory_update_scores(ws->global, key, e->recall_hits, e->recall_misses);
    /* Delete from workspace */
    memory_delete(ws->workspace, key);
    memory_find_free(e);
    return 0;
}
```

### 2.3 Tests for Phase 2

**File:** `tests/test_workspace.c` (extend from Phase 1)

```
test_workspace_promote_moves_entry()
  - Store "skill:test-promote" in workspace layer
  - Call workspace_promote(ws, "skill:test-promote")
  - Assert key NOT found in workspace layer (memory_find returns NULL)
  - Assert key IS found in global layer
  - Assert value matches original

test_workspace_promote_preserves_scores()
  - Store entry in workspace, set recall_hits=5, recall_misses=1
  - Promote it
  - Read from global layer, assert recall_hits=5, recall_misses=1

test_workspace_promote_nonexistent_fails()
  - Call workspace_promote(ws, "skill:does-not-exist")
  - Assert returns -1

test_workspace_promote_already_global_fails()
  - Store entry in global layer only
  - Call workspace_promote(ws, key)
  - Assert returns -1 (not in workspace layer)
```

**Playbook test (integration):**

```sh
# Manual/scripted integration test:
# 1. Create a workspace with 4+ skills, some with high recall_hits
# 2. Run: nash dream
# 3. Verify promoted skills appear in global memory dir
# 4. Verify non-generic skills were NOT promoted
```

---

## Phase 3: Curated Baseline Generic Skills

### Goal

Ship a set of proven generic coding skills that bootstrap the global memory
for new installations or sparse workspaces.

### 3.1 Create baseline skills directory

**New directory:** `data/generic-skills/`

Each file is a JSON memory entry matching the on-disk format:

```
data/generic-skills/skill__verify-before-edit.json
data/generic-skills/skill__save-findings-incrementally.json
data/generic-skills/skill__test-after-change.json
data/generic-skills/skill__read-errors-fully.json
data/generic-skills/skill__use-grep-before-reading.json
data/generic-skills/skill__check-build-after-edit.json
data/generic-skills/skill__predict-before-acting.json
data/generic-skills/skill__structured-scratchpad.json
data/generic-skills/skill__rollback-on-failure.json
data/generic-skills/skill__small-incremental-changes.json
```

Example entry:

```json
{
  "key": "skill:verify-before-edit",
  "value": "Always read a file's current content before editing it. Use file_read to verify the exact text you plan to replace exists at the expected location. Stale assumptions about file content are the #1 cause of failed edits.",
  "pinned": 0,
  "created_at": 0,
  "access_count": 0,
  "recall_hits": 0,
  "recall_misses": 0
}
```

### 3.2 First-run seeding

**File:** `src/setup.c` or `src/memory.c`

During `memory_new()` or a dedicated `memory_seed_defaults()`:

1. Check if global memory dir has a `.seeded` marker file
2. If not, read each JSON from `NASH_DATADIR/generic-skills/`
3. Store each via `memory_store()` into global memory
4. Create `.seeded` marker

```c
int memory_seed_defaults(memory_t *global_mem, const char *datadir) {
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/.seeded", global_mem->dir);
    if (access(marker, F_OK) == 0) return 0;  /* already seeded */

    char skills_dir[PATH_MAX];
    snprintf(skills_dir, sizeof(skills_dir), "%s/generic-skills", datadir);
    /* iterate *.json files, parse, memory_store() each */
    /* ... */

    /* Write marker */
    FILE *f = fopen(marker, "w");
    if (f) { fprintf(f, "seeded\n"); fclose(f); }
    return 0;
}
```

### 3.3 Install path

**File:** `Makefile`

Add to install target:

```makefile
install -d $(DESTDIR)$(NASH_DATADIR)/generic-skills
install -m 644 data/generic-skills/*.json $(DESTDIR)$(NASH_DATADIR)/generic-skills/
```

### 3.4 Tests for Phase 3

**File:** `tests/test_memory.c` (extend)

```
test_memory_seed_defaults()
  - Create temp dir, populate with 2-3 test JSON skill files
  - Create a fresh memory_t for "global"
  - Call memory_seed_defaults(global, temp_dir)
  - Assert all skills are now queryable
  - Assert .seeded marker exists

test_memory_seed_defaults_idempotent()
  - Call memory_seed_defaults() twice
  - Assert no duplicate entries (count unchanged)
  - Assert .seeded marker checked

test_memory_seed_skills_content()
  - After seeding, query for each expected skill key
  - Assert values are non-empty and match expected content
```

---

## Implementation Order and Dependencies

```
Phase 1.1  Add is_global to memory_entry_t           [memory.h]
Phase 1.2  Tag in workspace_recall()                  [workspace.c] depends on 1.1
Phase 1.3  Sparsity-aware injection                   [react_context.c] depends on 1.1, 1.2
Phase 1.4  Config option                              [config.h, config.c] independent
Phase 1.5  Tests                                      [tests/] depends on 1.1-1.4
  |
Phase 2.1  Dream playbook promotion pass              [dream.yaml] independent of Phase 1
Phase 2.2  workspace_promote() + CLI                  [workspace.c, commands.c] depends on 1.1
Phase 2.3  Tests                                      [tests/] depends on 2.1-2.2
  |
Phase 3.1  Create baseline skills JSON                [data/] independent
Phase 3.2  First-run seeding code                     [memory.c or setup.c] depends on 3.1
Phase 3.3  Makefile install                           [Makefile] depends on 3.1
Phase 3.4  Tests                                      [tests/] depends on 3.1-3.2
```

Phase 1 is the critical path. Phase 2 and Phase 3 can proceed in parallel
after Phase 1 is done.

---

## Files Modified (Summary)

| File | Phase | Change |
|------|-------|--------|
| src/memory.h | 1.1 | Add `is_global` to `memory_entry_t` |
| src/memory.c | 1.2 | Initialize `is_global=0` in query results |
| src/workspace.c | 1.2, 2.2 | Tag is_global in recall(); add workspace_promote() |
| src/workspace.h | 2.2 | Declare workspace_promote() |
| src/react_context.c | 1.3 | Sparsity-aware skill boost before inject_memory_type() |
| src/config.h | 1.4 | Add generic_skill_boost field |
| src/config.c | 1.4 | Parse and default generic_skill_boost |
| playbooks/dream.yaml | 2.1 | Add promotion pass (pass 5) |
| src/commands.c | 2.2 | Add `memory promote` CLI handler |
| data/generic-skills/*.json | 3.1 | 10 curated baseline skill files |
| src/memory.c (or setup.c) | 3.2 | memory_seed_defaults() function |
| Makefile | 1.5, 3.3 | Add test_workspace to TEST_BIN; install generic-skills |
| tests/test_workspace.c | 1.5, 2.3 | New test file for workspace layer ops |
| tests/test_memory.c | 1.5, 3.4 | Add is_global + seeding tests |

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| is_global field breaks existing code that zero-inits memory_entry_t | Low | Low | Field defaults to 0 (workspace), which is correct for non-workspace paths |
| Sparsity boost injects irrelevant global skills | Medium | Medium | Cap boost at 2x max_skills; config flag to disable |
| Dream promotion pass promotes project-specific skills | Medium | Medium | Conservative prompt constraints; max 5 per cycle |
| Seeded skills conflict with user's existing global entries | Low | Low | memory_store() updates existing entries; seeding only runs once (.seeded marker) |
| workspace_promote() loses metadata (validity, basis, etc.) | Medium | High | Copy ALL fields, not just value; test for preservation |

---

## Validation Criteria

Phase 1 is complete when:
- `make tests/test_workspace && ./tests/test_workspace` passes all 5 tests
- `make tests/test_memory && ./tests/test_memory` passes (including new test)
- Full `make` builds cleanly with no warnings
- Manual test: workspace with 0 local skills shows boosted global skill count
  in debug log

Phase 2 is complete when:
- `workspace_promote()` tests pass
- `nash dream` on a workspace with high-scoring skills results in promotion
- Promoted skills appear in `~/.nash/memory/` (global dir)

Phase 3 is complete when:
- Fresh `nash` install seeds global memory with baseline skills
- `memory_seed_defaults()` tests pass
- `make install` installs JSON files to `$(NASH_DATADIR)/generic-skills/`
- Second run does not re-seed (idempotency)
