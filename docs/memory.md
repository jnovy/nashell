# Memory Architecture

## Unified Memory Architecture (v4) -- Session-Centric Design

Nash v4 introduces a session-centric memory architecture built on three principles:

1. **Sessions are memory.** Every session records queries, tool calls, thoughts, errors, and results in `journal.jsonl`. Instead of building abstraction layers on top of sessions, make sessions themselves searchable.
2. **No automatic extraction.** Nothing enters persistent memory unless the agent or user explicitly stores it. The memory store stays small and high-quality.
3. **Store everything once, reference everywhere.** The content-addressed store (`.store/<sha256>`) and alias system (`R0S1`, `R1S3`) provide deduplication. Scratchpad uses append-only JSONL -- the same convention as `journal.jsonl`.

### Four Tiers

```
+-----------------------------------------------------+
|  L1: Context Window (volatile)                      |
|  Current conversation messages                      |
|  Managed by harness-1 compaction                    |
+-----------------------------------------------------+
|  L2: Scratchpad (session-persistent)                |
|  Append-only JSONL (scratchpad.jsonl)               |
|  Survives compaction, dies at session end            |
+-----------------------------------------------------+
|  L3: Session History (searchable, evolving)          |
|  sessions/<ts>/journal.jsonl + summary.txt + .emb   |
|  Grows, searchable via /? and memory_search         |
+-----------------------------------------------------+
|  L4: Curated Memory (persistent, small)             |
|  .memory/ -- only explicitly stored entries         |
|  Pinned entries always in context                   |
+-----------------------------------------------------+
```

Data moves **up** (L1->L4) through explicit agent action or natural session lifecycle. Data moves **down** (L4/L3->L1) through recall and injection. There is no automatic promotion pipeline.

### Data Flow

```
User query
    |
    v
react_build_context()
    +-- System prompt
    +-- Memory index + pinned (L4 -> L1)
    +-- Temporal event calendar (L4 timestamps -> L1)
    +-- Episodic recall (L3 session_index -> L1 journal chunks)
    +-- Semantic recall + associative graph walk (L4 -> L1)
    +-- Enriched rendering (recency, confidence, recall count)
    +-- Scratchpad (L2 -> L1, loaded from scratchpad.jsonl)
    +-- Previous result
    |
    v
React loop (tool calls)
    +-- Tools execute, results stored in .store/ with RXSX aliases
    +-- Working memory auto-promotion (tool results -> L2 scratchpad)
    +-- Error-triggered retrieval (L4 -> L1, query = error text)
    +-- Cycling-triggered retrieval (L4 -> L1, query = stuck action)
    +-- Scratchpad updated (in-memory sections)
    +-- Compaction fires when context full (harness-1)
    |   +-- Breadcrumbs for recoverable content
    |   +-- Eviction-triggered re-retrieval (L4 -> L1, query = breadcrumbs)
    |   +-- evicted_context -> scratchpad
    +-- Agent may call memory_search -> searches L4 + L3
    |
    v
done (task complete)
    |
    v
react_post_loop()
    +-- Validation scoring (recall_hits / recall_misses)
    +-- LLM reflection -> optional memory_store (L4)
    +-- Scratchpad pruning + save (append to scratchpad.jsonl)
    +-- journal_manifest() -> summary.txt + summary.emb
```

### Knowledge Formation

Sessions provide natural knowledge formation without automated pipelines:

1. Agent works on task, encounters problem X
2. `memory_search("problem X")` -> returns session matches (L3) since no curated memory exists yet
3. Agent reads the relevant session journal
4. Agent extracts the pattern and calls `memory_store` explicitly
5. Future tasks: `memory_search("problem X")` -> returns the stored pattern (L4 curated memory now ranks above raw session matches)

**Experience** (sessions) -> **Recognition** (search) -> **Crystallization** (memory_store)

---

## Persistent Memory System

Nash maintains a persistent, git-backed memory system that survives across sessions. Memories are categorized as **lessons** (what went wrong/right), **strategies** (reusable procedures), **skills** (domain-specific knowledge), **facts** (concrete data), **tasks** (ongoing work), **anti-patterns** (what not to do), and **other**.

### Hybrid Scoring -- Semantic + Substring + Bayesian Validation

Memory recall uses a composite scoring function that blends two signals:

```
relevance = semantic_similarity * blend_semantic + substring_match * blend_substring
composite = relevance
final_score = composite * pow(vscore, vscore_exponent)
```

The default blend weights are **40/60 semantic/substring**, favoring substring matching. This is grounded in [arXiv:2605.15184](https://arxiv.org/abs/2605.15184) ("Is Grep All You Need?"), which found that substring/grep-based retrieval outperforms vector search for **inline delivery** -- the delivery mode nash uses exclusively for memory injection. Nash memories contain literal spans (function names, error codes, file paths) where exact matching provides the strongest retrieval signal.

Importance (log access frequency) was intentionally removed from ranking because it distorted results -- boosting frequently-recalled but irrelevant memories above less-popular but more relevant ones. Importance is redundant with vscore: popular memories accumulate more recall hits -> higher vscore, which already captures usefulness without the distortion.

Where `vscore` is a **Bayesian validation score** using Beta posterior mean with Laplace smoothing:

```
vscore = (recall_hits + 1) / (recall_hits + recall_misses + 2)
```

Blend weights (`blend_semantic`, `blend_substring`) and `vscore_exponent` are exposed as self-harness tunable surfaces.

New memories start at vscore=0.5 (maximum entropy). Memories that consistently correlate with task failures get demoted.

### Vscore Exponent -- Cold-Start Correction

Full multiplicative application of vscore (`composite x vscore`) creates a **cold-start catch-22**: new memories get vscore=0.5, halving their composite score, making them less likely to be recalled, so they never accumulate evidence to escape vscore=0.5. Empirically, after 527 sessions, 86% of 639 memories were stuck at vscore=0.5 (zero evidence), while 7% with vscore>=0.90 enjoyed rich-get-richer dynamics.

The fix is a **power-law exponent**: `final_score = composite x pow(vscore, alpha)` where `alpha` (the `vscore_exponent` config parameter) defaults to 0.3.

| Exponent | vscore=0.50 (new) | vscore=0.33 (poor) | vscore=0.95 (veteran) | Effect |
|----------|-------------------|--------------------|----------------------|--------|
| **0.0** | x1.00 | x1.00 | x1.00 | Disabled -- pure relevance ranking |
| **0.3** (default) | x0.81 | x0.72 | x0.99 | Mild cold-start penalty; still penalizes actual misses |
| **1.0** | x0.50 | x0.33 | x0.95 | Original behavior -- harsh cold-start penalty |

With the default exponent=0.3, a new but relevant memory (relevance=0.40, vscore=0.50 -> 0.40x0.81=0.32) correctly beats a veteran but less relevant memory (relevance=0.25, vscore=0.95 -> 0.25x0.99=0.25). With exponent=1.0, the veteran would win (0.25x0.95=0.24 vs 0.40x0.50=0.20) despite lower relevance.

The scoring research foundations:

- **MemFail** [arXiv:2605.26667] -- diagnostic benchmark showing that injecting weakly-relevant memories *hurts* performance. Bayesian scoring provides the data-driven signal to identify which memories are genuinely useful.
- **Generative Agents** [Park et al., 2023] -- composite scoring (recency x importance x relevance) as the foundation for memory retrieval ranking.
- **Memory Survey** [arXiv:2404.13501] -- comprehensive survey identifying five critical memory operations, including validation/reflection as essential for memory quality.

### Memory Abstention Gate

Memories scoring below `recall_min_score` are **not injected**, implementing "abstention" -- the system stays silent when no stored experience is relevant. This prevents noise injection that hurts performance.

- **Mem-pi** [arXiv:2605.21463] -- generative memory policy that learns to abstain 30-40% of the time, yielding +22% avg improvement.

### Semantic Embeddings

When configured, nash uses dense vector embeddings for semantic similarity:

- **ONNX Runtime** -- local inference with models like `all-MiniLM-L6-v2` (no API calls needed)
- **Ollama** -- embedding via local Ollama server
- **OpenAI** -- embedding via OpenAI API

Cosine similarity is clamped to [0, 1] (negative = no match) and scaled to [0, 4] before blending with substring scores. Without embeddings, pure substring matching is used and normalized to the same [0, 1] range.

Embedding infrastructure optimizations:

- **ONNX batch inference** -- processes up to 32 texts at once via `onnx_embed_text_batch()` for 2-3x speedup over sequential embedding
- **Auto-detect threads** -- ONNX thread count auto-detected from CPU cores, clamped to [2, 8] (diminishing returns beyond 8 for small embedding models)
- **Paragraph-aware chunking** -- text is split hierarchically at paragraph boundaries (`\n\n`), then sentence boundaries (`. `), then word boundaries, producing semantically coherent chunks
- **Background backfill** -- session index embedding runs in a detached pthread at startup so the TUI is responsive immediately; uses a separate ONNX session for thread safety

### Memory Pruning -- Bayesian Quality Control

After every react loop, nash runs deterministic Bayesian pruning:

```
if vscore < prune_min_score AND evidence >= prune_min_evidence:
    delete memory
```

This removes memories that have accumulated enough evidence (default: 3 recalls) to demonstrate they're not useful (vscore below threshold). Inspired by:
- **MemMorph** [arXiv:2605.26154] -- showed that raw storage is insufficient; memories need active quality management

### Dreaming -- Tier 1 Consolidation

After every react loop, nash performs "dreaming" -- a lightweight consolidation pass:

- **Bayesian pruning** of low-quality memories
- **Deduplication** via embedding similarity
- **Consolidation** of related memories

Inspired by:
- **"Language Models Need Sleep"** [arXiv:2605.26099] -- dreaming/consolidation as essential for long-term memory health
- **CODESKILL** [arXiv:2605.25430] -- RL-trained skill extraction from task completions
- **MUSE-Autoskill** [arXiv:2605.27366] -- self-evolving skill library
- **TriMem** [arXiv:2605.19952] -- three-tier memory architecture (working/episodic/semantic)

### Dream Reminder

At startup, nash counts memory entries created since the last dream (using `created_at` timestamps vs `.last_dream` file mtime). If the count exceeds `dream_reminder_threshold`, a warning appears in the TUI status bar. This is usage-based, not calendar-based -- adapts to burst vs. quiet periods.

### Cue-Anchored Content-Pattern Triggers

Memories can declare substring patterns that cause automatic injection when matched in tool I/O. When storing a memory, pass a `triggers` array of up to 32 patterns:

```json
{"key": "lesson:cli-flag-ordering", "value": "...", "triggers": ["CLI flag", "argument parsing"]}
```

After each tool call, the harness scans all memories with triggers. If any pattern case-insensitively matches the tool output text (`strcasestr`), the memory is auto-injected as a `[CUE-ANCHORED MEMORY - triggered by: <pattern>]` message, capped at 2 injections per react step.

A **fire ledger** provides per-session deduplication - once a memory has been trigger-injected, it is added to the ledger and skipped on subsequent tool calls within the same react loop. The ledger resets between react steps.

### Event-Driven Reactive Retrieval

Nash implements **event-driven memory retrieval** -- the harness automatically re-queries memory when runtime events signal that new knowledge is needed. This directly addresses the **retrieval-timing bottleneck** identified in [arXiv:2605.30621](https://arxiv.org/abs/2605.30621): a single retrieval at task start creates a timing mismatch because the agent's needs evolve as it discovers what the task requires.

Four event triggers fire independently, each using the event content as the retrieval query (providing high signal-to-noise ratio):

| Trigger | Query Source | Injection Label | Config |
|---------|-------------|-----------------|--------|
| **Tool error** | Error text + action name | `[MEMORY HINT -- relevant to this error]` | `error_recall_*` |
| **Context eviction** | Breadcrumb summary of evicted messages | `[MEMORY RECOVERY -- post-eviction]` | `eviction_recall_*` |
| **Cycling detection** | Repeated action + path ("stuck cycling: ...") | `[MEMORY HINT -- you may be stuck]` | `cycling_recall_*` |
| **Content-pattern trigger** | Matching `triggers[]` substring in tool I/O | `[CUE-ANCHORED MEMORY - triggered by: <pattern>]` | per-memory `triggers` array |

All triggers follow the same pattern: query memory -> filter by relevance threshold -> deduplicate against `recalled_keys[]` -> inject as labeled user message -> track for validation scoring. This is the **Memory-as-Cognition** principle from [MemCog](https://arxiv.org/abs/2605.28046) -- the harness controls ALL retrieval timing; the LLM never decides when to recall.

Design rationale for event-driven over periodic retrieval: periodic re-retrieval (every N steps) was evaluated and rejected -- the schedule has zero correlation with actual retrieval need. Event-driven triggers provide signal-correlated retrieval where the event content IS the optimal query.

Based on:
- [arXiv:2605.30621](https://arxiv.org/abs/2605.30621) -- retrieval timing as the bottleneck in memory-augmented agents
- [MemCog](https://arxiv.org/abs/2605.28046) -- Memory-as-Cognition paradigm: navigable memory store with proactive reasoning
- [CogniFold](https://arxiv.org/abs/2605.13438) -- always-on proactive memory via cognitive folding

### Temporal Event Calendar

At context construction time, nash injects a `[TEMPORAL CONTEXT]` section that provides a chronologically-structured overview of recent memory activity. Memories are grouped into "Recent (last 7 days)" and "Older (last 30 days)" buckets, sorted most-recent-first, showing date, key, and description per entry.

This implements the **most impactful single component** identified in [arXiv:2605.15184](https://arxiv.org/abs/2605.15184) ("Is Grep All You Need?") -- removing the temporal events calendar halved accuracy for weaker models. The calendar provides temporal scaffolding that enables the model to reason about chronological relationships ("what changed recently?", "when did I learn this?").

```
[TEMPORAL CONTEXT]
Recent (last 7 days):
  2026-06-20  lesson:context-compaction-fixes -- Six context compaction flaws fixed
  2026-06-19  skill:implement-reactive-retrieval -- Adding auto retrieval to react loops

Older (last 30 days):
  2026-06-01  fact:nash-memory-store-size -- 314 entries after pruning
  2026-05-28  lesson:memory-retrieval-timing -- Bottleneck is timing not storage
```

Config: `temporal_calendar=true`, `temporal_recent_days=7`, `temporal_older_days=30`, `temporal_max_entries=20`.

### Episodic Recall from Session History

During context construction, nash queries the session index (`session_index_search()`) with the user query embedding to find similar past sessions. The best-matching journal chunks are injected as `[RECALLED SESSION CHUNK]` messages, providing raw problem-solving traces from prior experience.

This unlocks the full journal history during execution. Journal chunks contain actual tool sequences, error patterns, and solutions -- episodic memory that complements the distilled knowledge in L4 curated memories. The session index infrastructure (chunk-level embeddings, MaxSim retrieval) already existed for `session_grep` but was not queried during `react_build_context()`.

Inspired by [ByteRover](https://arxiv.org/abs/2604.01599) (agent-native memory with zero external infrastructure) and [MemMachine](https://arxiv.org/abs/2604.04853) (ground-truth-preserving episodic memory).

Config: `episodic_recall=true`, `episodic_max_results=2`, `episodic_min_score=0.35`.

### Associative Graph Walk

After semantic recall, nash follows `refs[]` links on recalled memories one level deep. When a recalled memory references other memories via its `refs` array, those referenced entries are looked up via `memory_find()` and injected as `[ASSOCIATED MEMORIES]` if they pass the relevance threshold.

The `refs[]` infrastructure has existed since the dreaming/consolidation system was implemented (populated by the SYNTHESIZE pass), but refs were previously only used for a +0.3 score boost during recall. This change actually injects the referenced content, implementing depth-1 associative navigation.

Inspired by [MRAgent](https://arxiv.org/abs/2606.06036) (ICML 2026 -- graph memory with iterative exploration, +23% on LoCoMo/LongMemEval) and [MemCog](https://arxiv.org/abs/2605.28046) (navigable memory store with associative link graphs).

Config: `associative_depth=1` (0 = disabled).

### Enriched Memory Rendering

Recalled memories are rendered with temporal and confidence metadata, not just bare key-value pairs:

```
--- skill:c-codebase-analysis-order (2d ago, 327 recalls, confidence: 95%) ---
## When to apply
When analyzing a C codebase for the first time...
```

Recency is computed from the `created_at` timestamp. Confidence uses the Beta posterior mean: `(hits + 1) / (hits + misses + 2) x 100%`. Recall count is the raw `recall_hits` value.

This implements the key finding from [arXiv:2605.15184](https://arxiv.org/abs/2605.15184) that **rendering IS retrieval** -- how memories are presented to the model matters as much as which ones are retrieved. The metadata helps the model weight recalled knowledge appropriately ("this has been recalled 327 times with 95% confidence" vs. "this was created yesterday with no validation").

### Working Memory Auto-Promotion

After successful tool execution with substantial output (>500 chars), the harness automatically appends a snippet to the scratchpad `auto_findings` section at priority 4. This implements the **always-on** principle from [CogniFold](https://arxiv.org/abs/2605.13438) -- memory operates without explicit agent action.

The total `auto_findings` section is capped at 2000 chars to prevent bloat (oldest entries dropped via FIFO). This replaces the diversity nudge hack ("consider saving findings to scratchpad") with harness-side cognitive offloading -- the agent doesn't need to remember to call `notes()`.

Config: `auto_promote=true`, `auto_promote_min_length=500`, `auto_promote_max_chars=2000`.

### Layered Workspaces -- Memory Segregation

Nash supports **workspace-based memory isolation** to prevent cross-contamination between different contexts (work, personal, hobby projects). The architecture uses two layers:

```
~/.nash/
+-- memory/                     <- GLOBAL layer (shared knowledge)
|   +-- skill:git-rebase.json
|   +-- lesson:file-edit.json
+-- workspaces/
    +-- work-acme/
    |   +-- memory/             <- WORKSPACE layer (work-only)
    +-- personal/
        +-- memory/             <- WORKSPACE layer (personal-only)
```

| Layer | Scope | Recall Behavior |
|-------|-------|-----------------|
| **Global** (`~/.nash/memory/`) | Universal skills & lessons | Always searched (unless `--isolated`) |
| **Workspace** (`~/.nash/workspaces/<name>/memory/`) | Project/context-specific | Only searched when that workspace is active |

**Two `memory_t` instances, not one with filtering.** Each workspace layer has its own index, mutex, git repo, and embedding cache -- providing hard filesystem isolation rather than soft prefix-based filtering.

**Recall merging:** Workspace results are scored first. If not isolated, global results are also retrieved and discounted by `global_recall_weight` (default 0.8), then merged and re-sorted by relevance.

**Store routing:** New memories go to the workspace by default. The `memory_store` tool accepts an optional `global` parameter to store directly to the global layer.

**Promotion/demotion:** `memory_promote` moves an entry from workspace -> global (for universally useful lessons). `memory_demote` moves from global -> current workspace.

**Backward compatible:** With no workspace configured, nash behaves exactly as before -- global-only mode with a single memory pool.

---

## Session History Search (L3)

Every completed session generates a searchable summary by reusing `journal_manifest()` -- which already exists and handles every edge case. The manifest is a compact digest (typically 500-2000 chars) containing query text, tool names, file paths, error markers, and done results -- exactly the content that embeds well for semantic search.

```
sessions/<timestamp>/
    journal.jsonl       # every tool call, params, results, errors
    summary.txt         # journal_manifest() output
    summary.emb         # embedding vector for semantic search
    scratchpad.jsonl    # append-only section data
```

At startup, nash builds an in-memory index of all sessions with embeddings. Session search uses cosine similarity with a gentle logarithmic recency boost:

```
recency = 1.0 / (1.0 + log1p(age_days / 30.0))
score = semantic_similarity * recency
```

### Unified Recall

There is no separate `session_search` tool. Instead, `memory_search` queries **both** L4 (curated memory) and L3 (session history) in a single call. Results are labeled by source:

```
[RECALLED MEMORY -- lesson:segfault-null-check]
Always check return value of malloc() before dereferencing...

[RECALLED SESSION -- 2026-06-15 14:23, nash (claude-sonnet-4)]
[Query R0] "fix the segfault in foo.c"
  + R0S1: file_read "foo.c" ...
  Result: Fixed null pointer dereference at line 73
  -> file_read sessions/1750000123.45678/journal.jsonl for details
```

The agent doesn't need to decide "should I search memory or sessions?" -- one call gets the best answer from wherever it lives. An optional `source` parameter (`all`/`memory`/`sessions`) exists for rare cases where filtering is needed.

### User Command: `/? query`

Users can search session history directly:

```
/? how did I fix the segfault
```

Results display in the TUI with session timestamp, model, query, result, and relevance score. The user can then navigate to that session for full details.
