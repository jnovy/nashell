# Implementation Plan: Failed State Isolation + Validated Knowledge Graph

Two high-impact improvements derived from ACID-Agent (arXiv 2608.13900) and
SodaMem (arXiv 2608.08055). Nash already has 90% of the infrastructure -
these are surgical extensions to existing systems.

---

## Part A: Failed State Isolation (TAINTED Importance Tier)

### Problem

Every failed tool call gets `LLM_MSG_IMPORTANCE_LOW` (react.c:641) - the same
tier as stale hints and degraded preamble. The assistant message containing the
flawed reasoning that *caused* the failure stays at `NORMAL`. This means:

1. Failed exchanges compete equally with useful LOW-priority content during
   eviction
2. The bad reasoning that led to the failure persists at NORMAL, potentially
   anchoring the model to repeat the same mistake
3. ACID-Agent paper shows +11.7% benchmark improvement from isolation alone

### Design

Add `LLM_MSG_IMPORTANCE_TAINTED = -1` below `LOW = 0`. The existing eviction
scoring formula (`score = imp * 100 - rec * 10 - size_bonus + pos_norm +
semantic_bonus`) automatically gives TAINTED messages a -100 base score vs
LOW's 0 - they are guaranteed to be evicted first without any eviction logic
changes.

### Changes (6 files, ~30 lines)

#### 1. llm.h:79 - Add TAINTED to importance enum

```c
typedef enum {
  LLM_MSG_IMPORTANCE_TAINTED = -1, /* failed tool results - evict first */
  LLM_MSG_IMPORTANCE_LOW = 0,      /* stale hints - evict early */
  LLM_MSG_IMPORTANCE_NORMAL = 1,   /* regular tool results - default */
  LLM_MSG_IMPORTANCE_HIGH = 2,     /* recent results, grep - compress first */
  LLM_MSG_IMPORTANCE_CRITICAL = 3  /* system, user query, scratchpad - never */
} llm_msg_importance_t;
```

No downstream breakage: eviction already uses `>= HIGH` as the protection
floor (react_eviction.c:656,999). TAINTED at -1 falls below that, handled
correctly as a candidate.

#### 2. react.c:641 - Failures return TAINTED

```c
if (!success) return LLM_MSG_IMPORTANCE_TAINTED;
```

This single line change makes every failed tool result score 100 points lower
than the next-lowest tier in eviction.

#### 3. react.c:1033-1034 - Cycling detection uses TAINTED

When the model is cycling (repeating the same tool call), both the assistant
message and tool result already get demoted to LOW. Change to TAINTED:

```c
chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_TAINTED;
chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_TAINTED;
```

#### 4. react.c (after tool result tagging) - Taint the reasoning too

After a failed tool call, also taint the assistant message that contained the
flawed reasoning. This is the key ACID-Agent insight - isolate the entire
failed *transaction* (reasoning + action + result), not just the result:

```c
/* After setting tool result importance */
if (!tr.success) {
  /* Taint the assistant message that preceded this failed tool call.
   * ACID-Agent (arXiv 2608.13900): isolating failed state (reasoning +
   * action + result) prevents contamination of future decisions. */
  if (chat->n_msgs >= 2)
    chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_TAINTED;
}
```

This is the critical piece. The model's reasoning that led to a `grep_search`
with a wrong pattern, or a `file_edit` with non-matching old_text, is
*actively harmful* in context - it anchors the model to repeat similar
mistakes.

#### 5. react_eviction.c - Optional: aggressive TAINTED sweep

In the mark phase, TAINTED messages already score lowest due to the formula.
But for an extra guarantee, add an early sweep before the main progressive
eviction:

```c
/* Phase 0: Sweep all TAINTED messages unconditionally.
 * Failed tool exchanges are pure noise - evict immediately rather than
 * letting them compete with useful LOW-priority content. */
for (int i = first_evictable; i < last_evictable; i++) {
  if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_TAINTED) {
    /* Generate breadcrumb before removal */
    evict_build_breadcrumb(chat, i, &crumbs);
    llm_chat_remove_msg(chat, i);
    did_evict++;
    i--; last_evictable--;
  }
}
```

This is optional - the scoring formula already handles it. But an explicit
sweep is faster (no scoring needed) and more predictable.

#### 6. Breadcrumb format for tainted messages

When generating breadcrumbs for evicted TAINTED messages, use a distinct
format so the model knows the evicted content was a failure:

```
[FAILED step N] tool_name: <first line of error> (evicted - failed state)
```

vs the current format for normal evictions:

```
[step N] tool_name: <summary>
```

### What NOT to taint

- Deduped results (react.c:1810) - stay LOW. Dedup is not failure.
- react.c:2007 context - need to verify but likely not failure-related
- `done()` tool calls - even failed done() attempts contain useful context
  about what the model thought was complete

### Expected Impact

ACID-Agent paper: +11.7% from isolation alone on SWE-bench Verified.
In Nash's architecture, this manifests as:
- Failed exchanges evicted first, freeing context for useful content
- Model less likely to repeat the same mistake (bad reasoning removed)
- Eviction is more predictable (TAINTED always goes first)

---

## Part C: Validated Knowledge Graph (Typed Edges + Validity Lifecycle)

### Problem

Nash's memory refs[] is an untyped `char**` - a flat list of related keys
with no semantics. All refs get the same +0.3 boost in scoring regardless
of relationship type. This means:

1. A SUPERSEDES ref boosts the superseded entry (wrong - should suppress it)
2. A CONTRADICTS ref boosts the contradicted entry (dangerous - amplifies
   conflicting information)
3. Superseded entries still appear in results at 30% score (soft demotion)
   rather than being excluded
4. Contradiction detection at store time only warns - never creates edges

SodaMem achieves 92.8% accuracy on LongMemEval-S by treating memory as a
temporal knowledge graph with typed edges and hard validity gates.

### Design: Three changes

#### C.1: Typed Memory Edges

Replace untyped `char **refs` with typed edges using a parallel array
approach (minimal struct change, backward compatible with existing JSON):

```c
/* memory.h - new edge type enum */
typedef enum {
  MEM_EDGE_RELATES = 0,      /* default, current behavior */
  MEM_EDGE_SUPERSEDES = 1,   /* this entry replaces the ref'd entry */
  MEM_EDGE_CONTRADICTS = 2,  /* this entry conflicts with the ref'd entry */
  MEM_EDGE_UPDATES = 3,      /* this entry refines/extends the ref'd entry */
  MEM_EDGE_DEPENDS = 4,      /* this entry requires the ref'd entry */
} mem_edge_type_t;

/* mem_index_entry_t changes */
typedef struct {
  /* ... existing fields ... */
  char **refs;              /* inter-memory ref keys (owned) */
  mem_edge_type_t *ref_types; /* parallel array: edge type per ref (owned) */
  int n_refs;
  /* ... */
} mem_index_entry_t;
```

Parallel array (not struct-of-arrays) for backward compatibility: existing
JSON without `ref_types` defaults all edges to MEM_EDGE_RELATES.

#### C.2: Edge-Aware Ref-Boost (memory.c:1241-1262)

Replace the flat +0.3 boost with edge-type-specific weights:

```c
/* Edge-type boost weights */
static const double edge_boost[] = {
  [MEM_EDGE_RELATES]     =  0.3,  /* current behavior */
  [MEM_EDGE_SUPERSEDES]  =  0.0,  /* don't boost what you replaced */
  [MEM_EDGE_CONTRADICTS] = -0.2,  /* actively suppress contradicted entries */
  [MEM_EDGE_UPDATES]     =  0.5,  /* boost what you extend */
  [MEM_EDGE_DEPENDS]     =  0.4,  /* boost what you need */
};

/* In the ref-boost loop (memory.c:1254) */
mem_edge_type_t etype = MEM_EDGE_RELATES;
if (ie->ref_types && ri < ie->n_refs)
  etype = ie->ref_types[ri];
double boost_weight = edge_boost[etype];
double boosted = scored[ref_map[slot].idx].score + boost_weight * scored[i].score;
if (boosted < 0.0) boosted = 0.0;
if (boosted > 1.0) boosted = 1.0;
scored[ref_map[slot].idx].score = boosted;
```

Key behaviors:
- SUPERSEDES: Entry A supersedes B. When A scores well, B gets +0.0 boost
  (no amplification of obsolete knowledge)
- CONTRADICTS: Entry A contradicts B. When A scores well, B gets -0.2
  penalty (active suppression of conflicting facts)
- UPDATES: Entry A updates B. When A scores well, B gets +0.5 boost
  (pull in the foundation you're building on)
- DEPENDS: Entry A depends on B. When A scores well, B gets +0.4 boost
  (pull in prerequisites)

#### C.3: Hard Validity Gate (memory.c:962)

Change superseded demotion from soft (30% score) to configurable hard
exclusion:

```c
/* memory.c score_entry_hybrid, line 962 */
if (superseded_at > 0.0 && superseded_demotion < 1.0f) {
  if (superseded_demotion <= 0.0f)
    return -1.0;  /* hard exclusion: superseded entries never returned */
  composite *= (double)superseded_demotion;
}
```

Add config option `memory.superseded_exclude` (boolean, default false for
backward compatibility). When true, set `superseded_demotion = 0.0f` in
memory_new() so superseded entries are hard-excluded from results.

The caller (memory_query) already filters by `composite >= min_score`, so
returning -1.0 from score_entry_hybrid is sufficient for exclusion - no
changes needed in the query loop.

#### C.4: Auto-Edge Creation at Store Time (tool_memory.c:489-532)

Upgrade the contradiction detection from warn-only to edge-creating:

```c
/* After detecting conflict (raw_relevance >= 0.60) */
if (n_conflicts > 0 && !supersedes) {
  /* Auto-create CONTRADICTS edges for high-similarity entries
   * that the user didn't explicitly supersede.
   * SodaMem: CONTRADICTS edges enable suppression at query time. */
  for (int ci = 0; ci < n_conflicts && ci < 3; ci++) {
    memory_add_ref(query_mem, key, conflict_keys[ci], MEM_EDGE_CONTRADICTS);
  }
}
```

This requires a new `memory_add_ref()` function:

```c
/* memory.h */
int memory_add_ref(memory_t *m, const char *key, const char *ref_key,
                   mem_edge_type_t edge_type);
```

#### C.5: Unify supersedes with typed refs

Currently `supersedes` is a separate field on mem_index_entry_t (line 48).
This creates a dual representation - some edges in `refs[]`, one edge in
`supersedes`. Unification approach:

Keep the `supersedes` field for backward compatibility and fast access, but
ALSO add a SUPERSEDES-typed ref when memory_set_supersedes is called:

```c
/* In memory_set_supersedes (memory.c:2025) - after existing logic */
memory_add_ref(m, new_key, old_key, MEM_EDGE_SUPERSEDES);
```

This way the ref-boost loop naturally handles supersession (0.0 boost to
the old entry), AND the dedicated supersedes field still works for the
lineage chain display and version tracking.

#### C.6: Tool API Changes (tool_memory.c)

Extend memory_store's `refs` parameter to accept typed edges:

Current format (backward compatible):
```json
{"refs": ["key1", "key2"]}
```

New format (typed):
```json
{"refs": [{"key": "key1", "type": "updates"}, {"key": "key2", "type": "contradicts"}]}
```

Detection: if refs[i] is a string, treat as MEM_EDGE_RELATES (backward
compatible). If refs[i] is an object with "key" and "type" fields, parse
the type.

### JSON Schema Changes

Memory entry JSON on disk gains optional `ref_types` array:

```json
{
  "key": "lesson:foo",
  "value": "...",
  "refs": ["lesson:bar", "skill:baz"],
  "ref_types": ["contradicts", "updates"],
  "supersedes": "lesson:old-foo",
  "superseded_at": 0.0
}
```

When `ref_types` is absent or shorter than `refs`, missing types default
to "relates". Full backward compatibility with existing memory stores.

---

## Implementation Order

### Sprint 1 (2-3 days): Failed State Isolation

1. Add TAINTED enum value to llm.h
2. Change react.c:641 to return TAINTED on failure
3. Taint assistant message on failure (react.c, after tool result)
4. Change cycling detection to use TAINTED (react.c:1033-1034)
5. Add TAINTED breadcrumb format in eviction
6. Test: verify TAINTED messages evict before LOW in progressive eviction

### Sprint 2 (3-4 days): Typed Memory Edges

1. Add mem_edge_type_t enum to memory.h
2. Add ref_types parallel array to mem_index_entry_t
3. Parse ref_types from JSON in memory loading (memory.c)
4. Write ref_types to JSON in memory storing (memory.c)
5. Implement memory_add_ref() function
6. Change ref-boost loop to use edge-type weights (memory.c:1254)
7. Update tool_memory.c refs parameter to accept typed edges
8. Test: verify edge-type-specific boost weights

### Sprint 3 (1-2 days): Validity Lifecycle

1. Add hard exclusion gate in score_entry_hybrid (memory.c:962)
2. Add memory.superseded_exclude config option
3. Auto-create CONTRADICTS edges in tool_memory.c store
4. Add SUPERSEDES ref in memory_set_supersedes
5. Test: verify superseded entries excluded when config enabled

### Sprint 4 (1 day): Integration

1. Verify all existing tests pass
2. Verify backward compatibility with existing memory stores
3. Load test with real memory database

---

## Risk Assessment

**TAINTED tier (Part A):** Very low risk. The enum is an int, -1 flows
through all existing comparisons correctly. The scoring formula produces
the right behavior automatically. Only risk: some code might compare
importance == 0 instead of importance == LOW, but grep shows no such
comparisons.

**Typed edges (Part C):** Low risk. Parallel array with NULL default means
existing code sees NULL ref_types and falls back to current behavior. The
ref-boost loop is isolated - changes are contained within the scoring
function.

**Hard exclusion (Part C.3):** Medium risk. Returning -1.0 from scoring
could surprise callers that don't expect negative scores. Need to verify
all callers of score_entry_hybrid handle negative returns as "exclude".
The memory_query loop at memory.c:1179 already filters by min_score, so
negative scores are automatically excluded. But test thoroughly.
