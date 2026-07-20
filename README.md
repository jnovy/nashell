# nash — Autonomous Coding Agent in C

**nash** is a fully autonomous coding agent implemented in C. It connects to any OpenAI-compatible LLM server (llama.cpp, OpenAI, Anthropic, Vertex AI) and executes multi-step coding tasks through a ReAct (Reason + Act) loop with persistent memory, a TUI interface, and research-grounded cognitive architecture.

Unlike wrapper-based agents, nash is a single compiled binary with zero Python dependencies. It runs locally with local models, maintains long-term memory across sessions, and learns from every task it completes.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                        TUI (ncurses)                    │
│  Markdown rendering · Step expansion · Keyboard nav     │
├─────────────────────────────────────────────────────────┤
│                     React Loop (react.c)                │
│  Plan → Tool Call → Observe → Reflect → Done            │
├──────────┬──────────┬───────────┬───────────────────────┤
│ Provider │  Memory  │   Tools   │   Journal + Store     │
│ local    │ semantic │ 18 tools  │ content-addressed     │
│ openai   │ Bayesian │ registry  │ full audit trail      │
│ anthropic│ event-   │ dispatch  │ checkpoint/resume     │
│ vertex   │ driven   │ filtering │ episodic recall       │
├──────────┴──────────┴───────────┴───────────────────────┤
│           Playbooks · Self-Harness · Model Profiles     │
├─────────────────────────────────────────────────────────┤
│              LLM Server (llama.cpp / API)                │
└─────────────────────────────────────────────────────────┘
```

---

## Unified Memory Architecture (v4) — Session-Centric Design

Nash v4 introduces a session-centric memory architecture built on three principles:

1. **Sessions are memory.** Every session records queries, tool calls, thoughts, errors, and results in `journal.jsonl`. Instead of building abstraction layers on top of sessions, make sessions themselves searchable.
2. **No automatic extraction.** Nothing enters persistent memory unless the agent or user explicitly stores it. The memory store stays small and high-quality.
3. **Store everything once, reference everywhere.** The content-addressed store (`.store/<sha256>`) and alias system (`R0S1`, `R1S3`) provide deduplication. Scratchpad uses append-only JSONL — the same convention as `journal.jsonl`.

### Four Tiers

```
┌─────────────────────────────────────────────────────┐
│  L1: Context Window (volatile)                      │
│  Current conversation messages                      │
│  Managed by harness-1 compaction                    │
├─────────────────────────────────────────────────────┤
│  L2: Scratchpad (session-persistent)                │
│  Append-only JSONL (scratchpad.jsonl)               │
│  Survives compaction, dies at session end            │
├─────────────────────────────────────────────────────┤
│  L3: Session History (searchable, evolving)         │
│  sessions/<ts>/journal.jsonl + summary.txt + .emb   │
│  Grows naturally, searchable via /? and memory_search│
├─────────────────────────────────────────────────────┤
│  L4: Curated Memory (persistent, small)             │
│  .memory/ — only explicitly stored entries          │
│  Pinned entries always in context                   │
└─────────────────────────────────────────────────────┘
```

Data moves **up** (L1→L4) through explicit agent action or natural session lifecycle. Data moves **down** (L4/L3→L1) through recall and injection. There is no automatic promotion pipeline.

### Data Flow

```
User query
    │
    ▼
react_build_context()
    ├── System prompt
    ├── Memory index + pinned (L4 → L1)
    ├── Temporal event calendar (L4 timestamps → L1)
    ├── Episodic recall (L3 session_index → L1 journal chunks)
    ├── Semantic recall + associative graph walk (L4 → L1)
    ├── Enriched rendering (recency, confidence, recall count)
    ├── Scratchpad (L2 → L1, loaded from scratchpad.jsonl)
    └── Previous result
    │
    ▼
React loop (tool calls)
    ├── Tools execute, results stored in .store/ with RXSX aliases
    ├── Working memory auto-promotion (tool results → L2 scratchpad)
    ├── Error-triggered retrieval (L4 → L1, query = error text)
    ├── Cycling-triggered retrieval (L4 → L1, query = stuck action)
    ├── Scratchpad updated (in-memory sections)
    ├── Compaction fires when context full (harness-1)
    │   ├── Breadcrumbs for recoverable content
    │   ├── Eviction-triggered re-retrieval (L4 → L1, query = breadcrumbs)
    │   └── evicted_context → scratchpad
    └── Agent may call memory_search → searches L4 + L3
    │
    ▼
done (task complete)
    │
    ▼
react_post_loop()
    ├── Validation scoring (recall_hits / recall_misses)
    ├── LLM reflection → optional memory_store (L4)
    ├── Scratchpad pruning + save (append to scratchpad.jsonl)
    └── journal_manifest() → summary.txt + summary.emb
```

### Knowledge Formation

Sessions provide natural knowledge formation without automated pipelines:

1. Agent works on task, encounters problem X
2. `memory_search("problem X")` → returns session matches (L3) since no curated memory exists yet
3. Agent reads the relevant session journal
4. Agent extracts the pattern and calls `memory_store` explicitly
5. Future tasks: `memory_search("problem X")` → returns the stored pattern (L4 curated memory now ranks above raw session matches)

**Experience** (sessions) → **Recognition** (search) → **Crystallization** (memory_store)

---

## Features

### ReAct Loop with Native Tool Calling

Nash implements a full ReAct (Reason + Act) loop that drives autonomous task completion:

```
User Query → [Plan] → Tool Call → Observe Result → [Reflect] → Next Tool Call → ... → Done
```

- **Native OpenAI tool_calls API** — uses structured `tool_calls` with `tool_call_id` threading, not JSON-in-content hacks
- **18 built-in tools** — shell_exec, file_read, file_write, file_edit, grep_search, glob_search, web_fetch, web_search, notes, plan, done, memory_store, memory_search, memory_pin, memory_unpin, memory_delete, image_analyze, user_ask
- **Shared tool registry** (`tools_registry.h`) — tool definitions defined once, formatted per-provider (local/OpenAI/Anthropic)
- **Dispatch table** — tool execution via function pointer table, not strcmp chains
- **Tool filtering** — per-playbook-pass whitelist/blacklist restricts available tools
- **Cycling detection** — detects repeated identical tool calls, injects corrective guidance, refuses after repeated failures
- **Concatenated tool name recovery** — when the model emits garbled names (e.g., `shell_execshell_exec`), automatically extracts the longest matching prefix and dispatches correctly
- **Unknown tool recovery** — when the model generates a non-existent tool name, injects a corrective message listing available tools and lets the model retry

### Multi-Provider Support

Nash supports four LLM providers through a unified vtable interface:

| Provider | Endpoint | Features |
|----------|----------|----------|
| **Local** (llama.cpp) | Any OpenAI-compatible server | `chat_template_kwargs`, `reasoning_budget` |
| **OpenAI** | `api.openai.com` | Strict mode (`additionalProperties: false`), prompt caching |
| **Anthropic** | `api.anthropic.com` | `input_schema` format, extended thinking, prompt caching |
| **Vertex AI** | Google Cloud | Anthropic-on-Vertex via `gcloud auth` token |

All providers share the same tool registry and SSE streaming infrastructure. Provider-specific differences (JSON structure, auth headers, error formats) are encapsulated in the vtable.

### Persistent Memory System

Nash maintains a persistent, git-backed memory system that survives across sessions. Memories are categorized as **lessons** (what went wrong/right), **strategies** (reusable procedures), **skills** (domain-specific knowledge), **facts** (concrete data), **tasks** (ongoing work), **anti-patterns** (what not to do), and **other**.

#### Hybrid Scoring — Semantic + Substring + Bayesian Validation

Memory recall uses a composite scoring function that blends two signals:

```
relevance = semantic_similarity * blend_semantic + substring_match * blend_substring
composite = relevance
final_score = composite * pow(vscore, vscore_exponent)
```

The default blend weights are **40/60 semantic/substring**, favoring substring matching. This is grounded in [arXiv:2605.15184](https://arxiv.org/abs/2605.15184) ("Is Grep All You Need?"), which found that substring/grep-based retrieval outperforms vector search for **inline delivery** — the delivery mode nash uses exclusively for memory injection. Nash memories contain literal spans (function names, error codes, file paths) where exact matching provides the strongest retrieval signal.

Importance (log access frequency) was intentionally removed from ranking because it distorted results — boosting frequently-recalled but irrelevant memories above less-popular but more relevant ones. Importance is redundant with vscore: popular memories accumulate more recall hits → higher vscore, which already captures usefulness without the distortion.

Where `vscore` is a **Bayesian validation score** using Beta posterior mean with Laplace smoothing:

```
vscore = (recall_hits + 1) / (recall_hits + recall_misses + 2)
```

Blend weights (`blend_semantic`, `blend_substring`) and `vscore_exponent` are exposed as self-harness tunable surfaces.

New memories start at vscore=0.5 (maximum entropy). Memories that consistently correlate with task failures get demoted.

#### Vscore Exponent — Cold-Start Correction

Full multiplicative application of vscore (`composite × vscore`) creates a **cold-start catch-22**: new memories get vscore=0.5, halving their composite score, making them less likely to be recalled, so they never accumulate evidence to escape vscore=0.5. Empirically, after 527 sessions, 86% of 639 memories were stuck at vscore=0.5 (zero evidence), while 7% with vscore≥0.90 enjoyed rich-get-richer dynamics.

The fix is a **power-law exponent**: `final_score = composite × pow(vscore, alpha)` where `alpha` (the `vscore_exponent` config parameter) defaults to 0.3.

| Exponent | vscore=0.50 (new) | vscore=0.33 (poor) | vscore=0.95 (veteran) | Effect |
|----------|-------------------|--------------------|----------------------|--------|
| **0.0** | ×1.00 | ×1.00 | ×1.00 | Disabled — pure relevance ranking |
| **0.3** (default) | ×0.81 | ×0.72 | ×0.99 | Mild cold-start penalty; still penalizes actual misses |
| **1.0** | ×0.50 | ×0.33 | ×0.95 | Original behavior — harsh cold-start penalty |

With the default exponent=0.3, a new but relevant memory (relevance=0.40, vscore=0.50 → 0.40×0.81=0.32) correctly beats a veteran but less relevant memory (relevance=0.25, vscore=0.95 → 0.25×0.99=0.25). With exponent=1.0, the veteran would win (0.25×0.95=0.24 vs 0.40×0.50=0.20) despite lower relevance.

The scoring research foundations:

- **MemFail** [arXiv:2605.26667] — diagnostic benchmark showing that injecting weakly-relevant memories *hurts* performance. Bayesian scoring provides the data-driven signal to identify which memories are genuinely useful.
- **Generative Agents** [Park et al., 2023] — composite scoring (recency × importance × relevance) as the foundation for memory retrieval ranking.
- **Memory Survey** [arXiv:2404.13501] — comprehensive survey identifying five critical memory operations, including validation/reflection as essential for memory quality.

#### Memory Abstention Gate

Memories scoring below `recall_min_score` are **not injected**, implementing "abstention" — the system stays silent when no stored experience is relevant. This prevents noise injection that hurts performance.

- **Mem-π** [arXiv:2605.21463] — generative memory policy that learns to abstain 30-40% of the time, yielding +22% avg improvement.

#### Semantic Embeddings

When configured, nash uses dense vector embeddings for semantic similarity:

- **ONNX Runtime** — local inference with models like `all-MiniLM-L6-v2` (no API calls needed)
- **Ollama** — embedding via local Ollama server
- **OpenAI** — embedding via OpenAI API

Cosine similarity is clamped to [0, 1] (negative = no match) and scaled to [0, 4] before blending with substring scores. Without embeddings, pure substring matching is used and normalized to the same [0, 1] range.

#### Memory Pruning — Bayesian Quality Control

After every react loop, nash runs deterministic Bayesian pruning:

```
if vscore < prune_min_score AND evidence >= prune_min_evidence:
    delete memory
```

This removes memories that have accumulated enough evidence (default: 3 recalls) to demonstrate they're not useful (vscore below threshold). Inspired by:
- **MemMorph** [arXiv:2605.26154] — showed that raw storage is insufficient; memories need active quality management

#### Dreaming — Tier 1 Consolidation

After every react loop, nash performs "dreaming" — a lightweight consolidation pass:

- **Bayesian pruning** of low-quality memories
- **Deduplication** via embedding similarity
- **Consolidation** of related memories

Inspired by:
- **"Language Models Need Sleep"** [arXiv:2605.26099] — dreaming/consolidation as essential for long-term memory health
- **CODESKILL** [arXiv:2605.25430] — RL-trained skill extraction from task completions
- **MUSE-Autoskill** [arXiv:2605.27366] — self-evolving skill library
- **TriMem** [arXiv:2605.19952] — three-tier memory architecture (working/episodic/semantic)

#### Dream Reminder

At startup, nash counts memory entries created since the last dream (using `created_at` timestamps vs `.last_dream` file mtime). If the count exceeds `dream_reminder_threshold`, a warning appears in the TUI status bar. This is usage-based, not calendar-based — adapts to burst vs. quiet periods.

#### Event-Driven Reactive Retrieval

Nash implements **event-driven memory retrieval** — the harness automatically re-queries memory when runtime events signal that new knowledge is needed. This directly addresses the **retrieval-timing bottleneck** identified in [arXiv:2605.30621](https://arxiv.org/abs/2605.30621): a single retrieval at task start creates a timing mismatch because the agent's needs evolve as it discovers what the task requires.

Four event triggers fire independently, each using the event content as the retrieval query (providing high signal-to-noise ratio):

| Trigger | Query Source | Injection Label | Config |
|---------|-------------|-----------------|--------|
| **Tool error** | Error text + action name | `[MEMORY HINT — relevant to this error]` | `error_recall_*` |
| **Context eviction** | Breadcrumb summary of evicted messages | `[MEMORY RECOVERY — post-eviction]` | `eviction_recall_*` |
| **Cycling detection** | Repeated action + path ("stuck cycling: ...") | `[MEMORY HINT — you may be stuck]` | `cycling_recall_*` |

All triggers follow the same pattern: query memory → filter by relevance threshold → deduplicate against `recalled_keys[]` → inject as labeled user message → track for validation scoring. This is the **Memory-as-Cognition** principle from [MemCog](https://arxiv.org/abs/2605.28046) — the harness controls ALL retrieval timing; the LLM never decides when to recall.

Design rationale for event-driven over periodic retrieval: periodic re-retrieval (every N steps) was evaluated and rejected — the schedule has zero correlation with actual retrieval need. Event-driven triggers provide signal-correlated retrieval where the event content IS the optimal query.

Based on:
- [arXiv:2605.30621](https://arxiv.org/abs/2605.30621) — retrieval timing as the bottleneck in memory-augmented agents
- [MemCog](https://arxiv.org/abs/2605.28046) — Memory-as-Cognition paradigm: navigable memory store with proactive reasoning
- [CogniFold](https://arxiv.org/abs/2605.13438) — always-on proactive memory via cognitive folding

#### Temporal Event Calendar

At context construction time, nash injects a `[TEMPORAL CONTEXT]` section that provides a chronologically-structured overview of recent memory activity. Memories are grouped into "Recent (last 7 days)" and "Older (last 30 days)" buckets, sorted most-recent-first, showing date, key, and description per entry.

This implements the **most impactful single component** identified in [arXiv:2605.15184](https://arxiv.org/abs/2605.15184) ("Is Grep All You Need?") — removing the temporal events calendar halved accuracy for weaker models. The calendar provides temporal scaffolding that enables the model to reason about chronological relationships ("what changed recently?", "when did I learn this?").

```
[TEMPORAL CONTEXT]
Recent (last 7 days):
  2026-06-20  lesson:context-compaction-fixes — Six context compaction flaws fixed
  2026-06-19  skill:implement-reactive-retrieval — Adding auto retrieval to react loops

Older (last 30 days):
  2026-06-01  fact:nash-memory-store-size — 314 entries after pruning
  2026-05-28  lesson:memory-retrieval-timing — Bottleneck is timing not storage
```

Config: `temporal_calendar=true`, `temporal_recent_days=7`, `temporal_older_days=30`, `temporal_max_entries=20`.

#### Episodic Recall from Session History

During context construction, nash queries the session index (`session_index_search()`) with the user query embedding to find similar past sessions. The best-matching journal chunks are injected as `[RECALLED SESSION CHUNK]` messages, providing raw problem-solving traces from prior experience.

This unlocks the full journal history during execution. Journal chunks contain actual tool sequences, error patterns, and solutions — episodic memory that complements the distilled knowledge in L4 curated memories. The session index infrastructure (chunk-level embeddings, MaxSim retrieval) already existed for `session_grep` but was not queried during `react_build_context()`.

Inspired by [ByteRover](https://arxiv.org/abs/2604.01599) (agent-native memory with zero external infrastructure) and [MemMachine](https://arxiv.org/abs/2604.04853) (ground-truth-preserving episodic memory).

Config: `episodic_recall=true`, `episodic_max_results=2`, `episodic_min_score=0.35`.

#### Associative Graph Walk

After semantic recall, nash follows `refs[]` links on recalled memories one level deep. When a recalled memory references other memories via its `refs` array, those referenced entries are looked up via `memory_find()` and injected as `[ASSOCIATED MEMORIES]` if they pass the relevance threshold.

The `refs[]` infrastructure has existed since the dreaming/consolidation system was implemented (populated by the SYNTHESIZE pass), but refs were previously only used for a +0.3 score boost during recall. This change actually injects the referenced content, implementing depth-1 associative navigation.

Inspired by [MRAgent](https://arxiv.org/abs/2606.06036) (ICML 2026 — graph memory with iterative exploration, +23% on LoCoMo/LongMemEval) and [MemCog](https://arxiv.org/abs/2605.28046) (navigable memory store with associative link graphs).

Config: `associative_depth=1` (0 = disabled).

#### Enriched Memory Rendering

Recalled memories are rendered with temporal and confidence metadata, not just bare key-value pairs:

```
--- skill:c-codebase-analysis-order (2d ago, 327 recalls, confidence: 95%) ---
## When to apply
When analyzing a C codebase for the first time...
```

Recency is computed from the `created_at` timestamp. Confidence uses the Beta posterior mean: `(hits + 1) / (hits + misses + 2) × 100%`. Recall count is the raw `recall_hits` value.

This implements the key finding from [arXiv:2605.15184](https://arxiv.org/abs/2605.15184) that **rendering IS retrieval** — how memories are presented to the model matters as much as which ones are retrieved. The metadata helps the model weight recalled knowledge appropriately ("this has been recalled 327 times with 95% confidence" vs. "this was created yesterday with no validation").

#### Working Memory Auto-Promotion

After successful tool execution with substantial output (>500 chars), the harness automatically appends a snippet to the scratchpad `auto_findings` section at priority 4. This implements the **always-on** principle from [CogniFold](https://arxiv.org/abs/2605.13438) — memory operates without explicit agent action.

The total `auto_findings` section is capped at 2000 chars to prevent bloat (oldest entries dropped via FIFO). This replaces the diversity nudge hack ("consider saving findings to scratchpad") with harness-side cognitive offloading — the agent doesn't need to remember to call `notes()`.

Config: `auto_promote=true`, `auto_promote_min_length=500`, `auto_promote_max_chars=2000`.

#### Layered Workspaces — Memory Segregation

Nash supports **workspace-based memory isolation** to prevent cross-contamination between different contexts (work, personal, hobby projects). The architecture uses two layers:

```
~/.nash/
├── memory/                     ← GLOBAL layer (shared knowledge)
│   ├── skill:git-rebase.json
│   └── lesson:file-edit.json
└── workspaces/
    ├── work-acme/
    │   └── memory/             ← WORKSPACE layer (work-only)
    └── personal/
        └── memory/             ← WORKSPACE layer (personal-only)
```

| Layer | Scope | Recall Behavior |
|-------|-------|-----------------|
| **Global** (`~/.nash/memory/`) | Universal skills & lessons | Always searched (unless `--isolated`) |
| **Workspace** (`~/.nash/workspaces/<name>/memory/`) | Project/context-specific | Only searched when that workspace is active |

**Two `memory_t` instances, not one with filtering.** Each workspace layer has its own index, mutex, git repo, and embedding cache — providing hard filesystem isolation rather than soft prefix-based filtering.

**Recall merging:** Workspace results are scored first. If not isolated, global results are also retrieved and discounted by `global_recall_weight` (default 0.8), then merged and re-sorted by relevance.

**Store routing:** New memories go to the workspace by default. The `memory_store` tool accepts an optional `global` parameter to store directly to the global layer.

**Promotion/demotion:** `memory_promote` moves an entry from workspace → global (for universally useful lessons). `memory_demote` moves from global → current workspace.

**Backward compatible:** With no workspace configured, nash behaves exactly as before — global-only mode with a single memory pool.

### Session History Search (L3)

Every completed session generates a searchable summary by reusing `journal_manifest()` — which already exists and handles every edge case. The manifest is a compact digest (typically 500–2000 chars) containing query text, tool names, file paths, error markers, and done results — exactly the content that embeds well for semantic search.

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

#### Unified Recall

There is no separate `session_search` tool. Instead, `memory_search` queries **both** L4 (curated memory) and L3 (session history) in a single call. Results are labeled by source:

```
[RECALLED MEMORY — lesson:segfault-null-check]
Always check return value of malloc() before dereferencing...

[RECALLED SESSION — 2026-06-15 14:23, nash (claude-sonnet-4)]
[Query R0] "fix the segfault in foo.c"
  + R0S1: file_read "foo.c" ...
  Result: Fixed null pointer dereference at line 73
  → file_read sessions/1750000123.45678/journal.jsonl for details
```

The agent doesn't need to decide "should I search memory or sessions?" — one call gets the best answer from wherever it lives. An optional `source` parameter (`all`/`memory`/`sessions`) exists for rare cases where filtering is needed.

#### User Command: `/? query`

Users can search session history directly:

```
/? how did I fix the segfault
```

Results display in the TUI with session timestamp, model, query, result, and relevance score. The user can then navigate to that session for full details.

### Scratchpad-Only Architecture (v5)

Nash uses a **scratchpad-only** architecture for cross-loop state management. Each react loop starts with a fresh context containing only:

```
[system]  System prompt (rules, tool definitions)
[user]    Memory (index + pinned + relevant skills)
[user]    Scratchpad (persistent working notes)
[user]    User query
```

No manifest. No last-exchange injection. No truncation. The scratchpad is the **sole** mechanism for passing state between react loops.

#### JSONL-Backed Persistence (L2)

Scratchpad sections are persisted to `scratchpad.jsonl` — an append-only JSONL file using the same convention as `journal.jsonl`. Each line is a self-contained JSON object; **last entry per section name wins**:

```jsonl
{"name":"findings","priority":1,"content":"The root cause is a null pointer..."}
{"name":"plan","priority":3,"content":"1. Add null check\n2. Add test\n3. Run suite"}
{"name":"findings","priority":1,"content":"Updated: confirmed null deref at line 73..."}
{"name":"status","op":"clear"}
```

This replaces the previous `<!-- priority:X -->` HTML comment format, which was fragile (invisible to LLMs, order-dependent parsing, round-trip loss). Benefits:

- **Metadata separation** — priority lives in JSONL fields, never in content. The LLM sees clean markdown; parsing is trivial `cJSON_Parse()` per line.
- **Crash safety** — append-only writes mean a crash mid-save loses at most the last partial line. All previously written sections survive intact.
- **History for free** — the JSONL file records every section update in order, useful for debugging or replaying scratchpad evolution.
- **Clean round-trip** — the LLM sees clean markdown without priority markers. When pruning output is parsed back, section names match and priorities are preserved from JSONL metadata.
- **Legacy fallback** — existing `scratchpad.md` files load via the legacy parser. New saves always use JSONL. Gradual migration with no flag day.

#### Context Eviction — Recoverability-Aware + Lossless Breadcrumbs

When context usage exceeds the eviction threshold (default 70%), nash uses a **multi-pass progressive eviction** pipeline inspired by two research papers:

**CWL (Context Window Lifecycle)** [arXiv:2606.11213]: Messages are annotated with a **recoverability level** based on their tool type:

| Recoverability | Tools | Eviction Priority |
|---------------|-------|-------------------|
| `RECOVER_FILE` | file_write, file_edit | Evict first (content persisted in filesystem) |
| `RECOVER_MEMORY` | memory_store, memory_pin | Evict first (content in long-term memory) |
| `RECOVER_SCRATCHPAD` | notes, plan | Evict early (content in scratchpad) |
| `RECOVER_STORE` | Any tool with store ref | Evict early (recoverable via `file_read`) |
| `RECOVER_NONE` | grep_search, web_fetch, etc. | Evict last (observations not persisted) |

During Pass 3 eviction, messages are sorted by `(importance ASC, recoverability DESC, age ASC)` — recoverable messages are evicted before non-recoverable ones at the same importance level.

**LCM-Lite (Lossless Context Management)** [arXiv:2605.04050]: Instead of losing evicted content, nash generates a **breadcrumb index** of evicted store refs:

```
[EVICTED CONTEXT — recoverable via file_read]
- R0S15: {"pattern":"eviction","path":"src/","matches":50... (user, 837 chars)
- R0S22: {"path":"src/memory.c","total_lines":1932... (user, 9915 chars)
```

The breadcrumb is injected after the scratchpad at the eviction point, giving the LLM awareness of what was evicted and how to recover it. Non-recoverable messages still get heuristic extraction into the scratchpad.

This replaces the old approach of uniform scratchpad dumping with a two-track system: recoverable content gets indexed, non-recoverable content gets summarized.

#### Post-Done Scratchpad Pruning

After each task completes, an LLM call removes resolved/completed information from the scratchpad:

> *"Remove ONLY information from the scratchpad that was resolved or completed by this task. Keep everything else exactly as-is — do not rewrite, merge, summarize, or reformat."*

The `extract_llm_text_output()` helper accepts both plain markdown and JSON tool-call format from the LLM, extracting the `content` field from JSON if the model outputs a tool call instead of plain text.

#### Auto-Save Done Results

When `done` is called, the result is automatically saved to the scratchpad as `R<N>_result` (priority 1), ensuring the next react loop has full access to the previous loop's conclusion.

### Structural Reasoning (Default)

Nash's default thinking mode is **structural reasoning** — a two-call pattern inspired by [arXiv:2603.05344] that separates reasoning from action:

1. **Call 1: Reason** — LLM is called with **no tool schemas** and thinking OFF. Without tools available, the model is structurally forced to produce plain-text analysis instead of jumping to action.
2. **Call 2: Act** — Tools are restored. The reasoning from Call 1 persists as an assistant message in context, informing the tool call.

Key advantages over native thinking (`on` mode):
- **Persistent reasoning** — native thinking vanishes after each turn; structural reasoning becomes a regular context message visible to all future steps
- **Provider-agnostic** — works with every provider (local, OpenAI, Anthropic, Vertex) with no API requirements
- **Faster & cheaper** — models produce concise analysis (500-1500 tokens) vs verbose native thinking (2000-8000 tokens)
- **Controllable** — reasoning depth is shaped by the prompt, not a token budget knob
- **Smaller request payload** — Call 1 has no tool schemas (~2-4K fewer input tokens)

The reasoning messages are marked LOW importance so they compress/evict early during context pressure, preventing accumulation.

Configuration:
```toml
[thinking]
mode = "on"              # off | on | structural (default: on)
budget = -1              # -1=unrestricted, 0=none, N>0=max thinking tokens
```

| Mode | Mechanism | Provider Requirement | Speed | Cost |
|------|-----------|---------------------|-------|------|
| `off` | No reasoning phase | Any | Fastest | Lowest |
| `on` | Native thinking API | Anthropic/OpenAI/llama.cpp | Slowest | Highest |
| **`structural`** | Two-call: reason (no tools) -> act | **Any** | Fast | Low |

### Harness-1 Context Management

Inspired by [Harness-1](https://arxiv.org/abs/2606.02373) (UIUC/Berkeley/Chroma, 2026), nash implements **stateful cognitive offloading** — moving context bookkeeping from the LLM to the environment. Instead of letting the model waste reasoning capacity on tracking what it has seen, managing duplicates, and deciding what to keep, the harness handles this automatically.

Six mechanisms adapted from the paper:

#### 1. Importance-Tagged Messages

Every message in the LLM context carries an importance level (`CRITICAL` / `HIGH` / `NORMAL` / `LOW`) auto-assigned by type:

| Importance | Message Types |
|------------|---------------|
| **CRITICAL** | System prompt, user query, scratchpad, `done` results |
| **HIGH** | `grep_search`, `memory_search`, recent assistant turns |
| **NORMAL** | `file_read`, `shell_exec`, `web_fetch` |
| **LOW** | Errors, hints, deduplicated results |

Importance controls eviction priority — `LOW` messages are stripped first, `CRITICAL` messages are never evicted.

#### 2. Multi-Pass Progressive Context Rendering

Replaces the previous single-pass binary eviction with 3-pass progressive degradation:

| Pass | Trigger | Action |
|------|---------|--------|
| **Pass 1** | Context > threshold | Strip `LOW` importance messages (errors, hints, deduped) |
| **Pass 2** | Still over threshold | Compress `NORMAL` messages to top-4 sentences via sentence-BM25 |
| **Pass 3** | Still over threshold | Standard eviction with scratchpad extraction + compaction floor |

This preserves more useful context at each stage instead of dropping messages wholesale.

#### 3. Sentence-BM25 Relevance Compression

New `compress.c` module (301 lines) implements relevance-based text compression:

- Splits text into sentences
- Filters stopwords (70 English + 20 programming terms) to prevent common words from drowning out semantically meaningful query terms
- Scores each sentence by BM25-like term overlap with the current query
- Keeps top-N sentences in their original order

Used in eviction Pass 2 instead of blind character-limit truncation. A `file_read` output that returned 200 lines gets compressed to the 4 most relevant sentences rather than keeping the first N characters.

#### 4. Context-Level Deduplication

CRC32 hash tracking in a 64-entry rolling buffer detects when the same content appears multiple times in context (e.g., reading the same file twice, overlapping `grep_search` results):

```
[dedup] Same as step 3 — see earlier result
```

Prevents wasting context on re-reading identical content.

#### 5. Auto-Seeding Scratchpad

The first successful tool result (step 0) automatically seeds the scratchpad at priority 3, ensuring it's never empty when eviction kicks in. This implements Harness-1's "warm-started curation" — the agent starts with something rather than a cold empty state.

#### 6. Tool Diversity Nudge

Tool usage tracking (`tool_use_counts[32]` in `tool_ctx_t`) monitors which tools the agent is using. After 10+ steps without calling `notes()` to save findings, a one-shot soft hint is injected:

> *Consider saving key findings to your scratchpad with notes() — they survive context eviction.*

This prevents the common failure mode of accumulating context without ever externalizing key findings.

**Files**: `src/llm.h`, `src/llm.c`, `src/tools.h`, `src/react.c`, `src/compress.c`, `src/compress.h`

### TUI — Terminal User Interface

Nash provides a full ncurses-based TUI with:

- **Markdown rendering** — headers, bold, italic, inline code, code blocks, tables, horizontal rules, lists
- **Inline formatting in links** — tool names rendered in bold, descriptions as inline code
- **Step expansion** — click/Enter on a step to expand its full content from the store
- **Streaming output** — real-time token display during LLM generation
- **Status bar** — model name, context usage percentage (`ctx 42%`), background jobs count, dream reminder
- **Journal view** — full session history with react loop headers, step markers (+/x), thoughts (💭)
- **Keyboard navigation** — arrow keys, Page Up/Down, Home/End, Enter to expand/collapse
- **Input history** — arrow keys browse query history; arrow-down past the last entry restores in-progress text (standard shell/readline behavior)
- **Pause/Resume** — press Space during inference to pause after the current step; Space or new query to resume
- **Auto-redirect** — typing a new query during active inference automatically pauses the current task, stashes the new query, and dispatches it immediately when the loop yields — no "Space then type" dance required
- **Cross-session search** — type `/?query` for incremental scratchpad search, or `/? query` for semantic session history search (embedding-based, searches `summary.emb` across all sessions)

#### TUI Slash Commands

| Command | Description |
|---------|-------------|
| `/dream` | Run memory consolidation (alias for `/play dream`) |
| `/play NAME` | Run a named playbook in background (e.g., `/play reflect`) |
| `/play list` | List all available playbooks with descriptions |
| `/fork N` | Fork the session at step N — copies journal, symlinks, and checkpoint to a new session |
| `/name NAME` | Create a named symlink to the current session (`~/.nash/sessions/NAME`) |
| `/cwd DIR` | Change working directory; creates the directory if it doesn't exist (`mkdir -p`) |
| `/runs` | List all playbook run logs (from `~/.nash/runs/`) |
| `/runs show ID` | Display details of a specific playbook run |
| `/memory_search QUERY` | Search memory using hybrid scoring; display ranked results in the TUI |
| `/ms QUERY` | Full-parameter memory search (alias: `/memory_search`). Supports `-q` query, `-k` key, `-p` pattern, `-r` regex, `-n` max results, `-d` days. Searches both curated memory (L4) and session journals (L3) |
| `/workspace NAME` | Switch to a named workspace mid-session; `/workspace` shows current workspace |
| `/?query` | Cross-session scratchpad search (live incremental results) |
| `/? query` | Semantic session history search (embedding-based, shows ranked results) |
| `/continue` | Resume from checkpoint with the original query |
| `quit` / `exit` | Exit nash |

#### Tree-Based Branching

Nash supports **non-linear conversation trees**. When the user views a previous react loop's output (`reactRX.md`) and submits a new query, nash branches from that point:

1. The `parent_loop` is set to the viewed react loop
2. A `[Branched from R<N>: "original query"]` context hint is injected
3. The parent loop's result is loaded from the scratchpad
4. Scratchpad sections are filtered non-destructively at injection time — only sections from the ancestor chain are included

This enables exploring alternative approaches without losing the original conversation path.

### Content-Addressed Store & Journal

Every tool output, error, and metadata entry is stored in a **content-addressed store** (`~/.nash/store/`) using SHA-256 hashing. Session directories contain symlinks (`R0S0`, `R0S1`, ...) pointing to store entries.

The **journal** (`journal.jsonl`) records every event with:
- React loop number and step
- Tool name and parameters
- Store reference (ref alias)
- Size, line count
- Failed flag and error message
- Tool call ID (for API threading)

All journal entries have store refs — no more `+ ?: memory_context` entries. Every step is viewable and inspectable in the TUI.

### Checkpoint/Resume

Nash saves a checkpoint after every tool execution:
```json
{
  "version": 1,
  "step": 15,
  "react_loop": 2,
  "user_query": "fix the memory leak in tools.c",
  "last_tc_id": "abc123..."
}
```

On crash or restart, nash detects the checkpoint and rebuilds the conversation from:
1. Fresh system prompt (may have changed)
2. Fresh memory context
3. Restored scratchpad (from disk)
4. Replayed journal entries (tool calls + compact results)

### Session Management

#### Named Sessions

Use `/name PROJECT` to create a human-readable symlink to the current session:
```
~/.nash/sessions/my-refactor → ~/.nash/sessions/1779970830.40871
```
Named sessions can be resumed with `--session ~/.nash/sessions/my-refactor`.

#### Session Forking

`/fork N` creates a new session branched at step N:
1. Journal entries up to step N are copied
2. Store symlinks (R0S0, R0S1, ...) are replicated
3. A checkpoint is written at the fork point
4. The TUI switches to the forked session

This enables exploring alternative strategies from any point in a session's history.

#### Lazy Sessions (Headless Mode)

In headless mode (`-p QUERY`), session directories are created lazily — only when the first journal entry is written. Empty sessions are automatically cleaned up on exit (`rmdir` if empty). This prevents clutter from quick queries that produce no artifacts.

#### Session Auto-Detection

In TUI mode, if the current working directory contains a `journal.jsonl`, nash resumes that session automatically. This is intentionally disabled in headless mode to prevent a child `nash -p` process (spawned via `shell_exec`) from hijacking its parent's session.

### Post-Task Reflection

After every completed task, nash runs a **reflection phase** — a mini react loop that extracts reusable lessons, strategies, and skills:

```
System: "Perform CAUSAL ANALYSIS (not narrative summary)..."
1. What assumptions held or almost failed?
2. What hidden variables or context mattered most?
3. What observations were initially ignored?
4. What search branches were pruned — correctly or incorrectly?
5. What representation or mental model was key to success?
6. What reusable invariant or principle generalizes beyond this task?
```

The model calls `memory_store` to persist lessons, then `done` to finish reflection. Failed tasks get a different prompt focused on failure analysis.

After reflection and scratchpad pruning, nash generates a searchable session summary by calling `journal_manifest()` and embedding the output as `summary.txt` + `summary.emb`. This makes the session discoverable via `memory_search` and `/? query` for all future sessions.

### Playbooks — Multi-Pass Task Orchestration

Playbooks are YAML-defined multi-pass workflows that orchestrate sequences of react loops with fine-grained control over each pass:

```yaml
name: dream
description: "Memory consolidation - 4-pass cognitive maintenance"
session_mode: per-pass      # fresh session per pass | shared
scratchpad_mode: shared      # carry scratchpad across passes | isolated
pause_between: false

react:                       # defaults for all passes
  max_steps: 0
  inject_memory: false
  enable_reflection: false

passes:
  - label: "Inventory & scan"
    react:
      tools_block: [memory_store, memory_delete]  # per-pass tool filter
    prompt: |
      Scan and catalog all memories at: {{memory_dir}}
```

Features:
- **Per-pass react overrides** — max_steps, memory injection, reflection, compaction, scoring
- **Tool filtering** — whitelist (`tools_allow`) or blacklist (`tools_block`) per pass
- **Template variables** — `{{memory_dir}}`, `{{model}}`, `{{session_dir}}`, `{{nash_dir}}`, custom vars
- **Session modes** — `per-pass` (fresh session each pass) or `shared` (one session)
- **Scratchpad modes** — `shared` (carry across passes) or `isolated` (fresh each pass)
- **Post hooks** — `prune_memory`, `commit`
- **Inter-pass pause** — optionally wait for user confirmation between passes

Bundled playbooks: `dream`, `reflect`, `digest`, `health`, `prune`, `retrospect`, `self-harness`

Run with `--play NAME` or from the TUI.

### Self-Harness — Automated Weakness Mining & Validation

Inspired by [Self-Harness, arXiv:2606.09498], nash includes a full self-improvement loop:

#### Postmortem Analysis (Weakness Mining)

Scans session journals to identify recurring failure patterns:

```bash
nash --postmortem                    # analyze last 50 sessions
nash --postmortem-sessions 100       # analyze last 100 sessions
```

Failure signatures are clustered by `(terminal_cause, mechanism, tool)`:
- **Terminal causes**: tool_error, step_limit, null_result, cycling, empty_result
- **Mechanisms**: file_edit_mismatch, unread_ref, shell_retry, context_eviction, wrong_tool, hallucination, spec_violation

Produces evidence bundles for LLM-driven proposal generation.

#### Step-Level Trajectory Scoring (SWE-Shepherd)

Inspired by SWE-Shepherd [arXiv:2604.10493], the postmortem now includes **step-level productivity scoring** — each tool call in a session is classified as:

| Score | Classification | Heuristic |
|-------|---------------|----------|
| +2 | **PRODUCTIVE** | Tool succeeded AND result was file_read'd or is inherently productive (done, notes, file_write, etc.) |
| +1 | **NEUTRAL** | Tool succeeded, result usage unclear |
|  0 | **WASTEFUL** | Tool succeeded but output ref was never read |
| -1 | **HARMFUL** | Tool failed (error returned) |
| -2 | **SPINNING** | 3+ consecutive identical tool+params (cycling) |

Aggregate metrics per session:
- **Efficiency** = productive steps / total steps
- **Waste ratio** = wasteful steps / total steps
- **Causal step** = earliest step in the longest harmful streak (failure attribution)

The evidence bundle now includes a "Trajectory Quality" section with aggregate stats across all analyzed sessions, making it possible to track efficiency trends over time.

#### Regression Testing (Validation Gate)

Query banks in `~/.nash/regression/` (YAML) define test queries with criteria:

```yaml
name: core-tools
split: held-in
queries:
  - id: file-read-basic
    query: "Read the first 10 lines of README.md"
    criteria:
      - type: status
      - type: tool_used
        expect: file_read
      - type: max_steps
        expect: "5"
```

Criterion types: `status`, `contains`, `not_contains`, `regex`, `tool_used`, `tool_not_used`, `max_steps`, `no_error`, `exit_code`

Validation gate implements the Self-Harness acceptance rule:
```
Δ_in ≥ 0 AND Δ_ho ≥ 0 AND max(Δ_in, Δ_ho) > 0
```

```bash
nash --regression                              # run all tests
nash --regression --split held-in              # held-in only
nash --validate-harness baseline               # save baseline
nash --validate-harness compare                # compare against baseline
```

#### Tunable Surfaces

Self-harness tunable parameters exposed in config:
- `recall_blend_semantic` / `recall_blend_substring` — memory scoring blend weights
- `vscore_exponent` — Bayesian validation power-law exponent
- `tool_retry_limit` — max consecutive errors before forced strategy switch
- `cycling_window` / `cycling_threshold` — cycling detection sensitivity

#### EvolveMem — Retrieval Quality Telemetry

Inspired by EvolveMem [arXiv:2605.13941], nash now logs **memory retrieval quality telemetry** after each task. A `memory_quality` journal entry records:

- Which memories were recalled (keys)
- Task outcome (success/failure)
- Cold-start count (memories with zero evidence, vscore=0.5)
- Cold-start percentage

The self-harness playbook includes a 4th pass ("Retrieval Quality Diagnosis") that scans these telemetry entries to diagnose retrieval configuration issues:
- High cold-start rate → decrease `vscore_exponent`
- Low success rate with recalled memories → increase `recall_min_score`
- Too many injections → decrease `max_*_per_query`

This closes the feedback loop between memory retrieval outcomes and retrieval configuration, enabling data-driven tuning of the retrieval parameters.

### Model Profiles — Per-Model Spec Overrides

Model profiles in `~/.nash/models/*.toml` provide **full configuration overrides** per model. When nash detects which model it's talking to (via `config_match_model()` longest-substring match), the matched profile overlays any `config_t` field using sentinel-based inheritance: unset fields (`-1`, `0.0`, `NULL`) inherit from `config.toml`, set fields override it.

This implements the [OpenJarvis](https://arxiv.org/abs/2605.17172) insight that **the model profile IS the local/cloud adapter** — no "local mode" toggle needed. Switching from Claude to Qwen automatically adjusts temperature, step limits, tool restrictions, memory thresholds, and tool descriptions.

#### Layer Resolution Order

```
compile-time defaults → config.toml → model profile → playbook pass → CLI flags
```

Each layer only overrides fields it explicitly sets. Everything else cascades from the layer below.

#### Example: Local Model (Qwen)

```toml
match = "qwen"
chars_per_token = 4.0
native_context = 131072

[thinking]
mode = "on"
budget = 8192                          # constrained -- local model

[client]
temperature = 0.5                      # lower = fewer hallucinations
max_tokens = 12288

[react]
max_react_steps = 30                   # cap steps — local models cycle more
cycling_detection = true
tool_retry_limit = 2                   # fail fast
max_reflection_steps = 3

[memory]
recall_min_score = 0.20                # stricter — inject fewer, more relevant
memory_index_max = 40
max_skills_per_query = 2
max_lessons_per_query = 2

[tools]
block = ["web_search"]                 # block tools the model can't use well

[tools.file_edit]
description = """Edit a file by replacing exact text. CRITICAL: You MUST call
file_read first and copy old_text character-by-character from the output."""

system_prompt_extra = """
[MODEL-SPECIFIC RULES — Qwen]
- Issue exactly ONE tool call per response.
- After a tool error, do NOT retry the same approach. Switch strategy immediately.
- When calling file_edit, copy old_text from file_read output verbatim.
"""
```

#### Example: Cloud Model (Claude)

```toml
match = "claude"
chars_per_token = 3.5
native_context = 200000

[thinking]
mode = "yes"
budget = -1                            # unlimited — cloud can afford it

# Cloud models are capable — light touch, inherit config.toml defaults
system_prompt_extra = """
[MODEL-SPECIFIC RULES — Claude]
- When using file_edit, always file_read first to verify the exact text.
"""
```

#### Overridable Fields

| Section | Fields | Sentinel |
|---------|--------|----------|
| `[client]` | `temperature`, `max_tokens` | `0.0` / `0` = inherit |
| `[thinking]` | `mode`, `budget` | `THINKING_UNSET` = inherit |
| `[react]` | `inject_memory`, `inject_prev_result`, `enable_reflection`, `enable_pruning`, `enable_compaction`, `enable_scoring` | `-1` = inherit |
| `[react]` | `max_react_steps`, `max_reflection_steps`, `tool_retry_limit`, `cycling_detection` | `0` / `-1` = inherit |
| `[memory]` | `recall_min_score`, `recall_blend_semantic`, `recall_blend_substring`, `vscore_exponent`, `memory_index_max`, `max_*_per_query`, `context_eviction_pct` | `0.0` / `0` / `-2.0` = inherit |
| `[tools]` | `allow` (whitelist), `block` (blacklist) | `NULL` = inherit (all tools) |
| `[tools.<name>]` | `description` — per-tool description override | `NULL` = use compiled default |

The special sentinel `-2.0` for `vscore_exponent` exists because both `0.0` (disabled) and `-1.0` are valid values.

#### Features
- **Longest-match priority** — `qwen3-30b` matches before `qwen` for model ID `qwen3-30b-a3b`
- **Playbook cascade** — profile overrides flow through to all playbook passes as the lowest-priority layer
- **Tool description overrides** — rewrite tool descriptions for weaker models without recompiling
- **Tool filtering** — whitelist or blacklist tools per model (small models can't compose complex tools)
- **Native context warnings** — alerts when server n_ctx is much smaller than model capacity

Example profiles for Claude, Qwen, LLaMA, DeepSeek, Gemma, and Mistral are shown above. Create them at `~/.nash/models/` to customize behavior per model.

### Prompt Optimization — `--optimize`

After creating a model profile, run `--optimize` to automatically tune the `system_prompt_extra` field for your specific model. This implements the Self-Harness iterative loop ([arXiv:2606.09498](https://arxiv.org/abs/2606.09498)):

1. **Weakness Mining** — cluster failures from regression runs by signature
2. **Harness Proposal** — generate K diverse, minimal candidate edits to `system_prompt_extra`
3. **Proposal Validation** — accept only non-regressive edits (held-in does not degrade, held-out improves)

```bash
nash --optimize light                          # 3 rounds (~4 regression runs)
nash --optimize medium                         # 6 rounds (~7 regression runs)
nash --optimize heavy                          # 10 rounds (~11 regression runs)
nash --optimize 5                              # explicit round count (1-50)
```

Additional flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--reflect-model provider/model` | student model | Separate LM for reflection (e.g. `anthropic/claude-sonnet-4-20250514`) |
| `--epochs N` | 1 | Multi-epoch training — repeat the full optimization N times |
| `--edit-budget N` | 4 | Initial edit budget L_0 (cosine decay to floor across rounds) |

The optimizer uses query banks from `~/.nash/regression/` (the same ones used by `--regression`). If no query banks exist, a seed set is auto-generated on first run.

#### Standard Workflow for Onboarding a Local Model

Running `--optimize` should be a **standard step** when onboarding any new local model — not an optional afterthought. Research on harness self-improvement ([RHI, arXiv:2607.15524](https://arxiv.org/abs/2607.15524)) found that a few optimization iterations substantially raise the performance ceiling of low-reasoning-effort agents, often **exceeding the maximum-reasoning-effort setting** while reducing inference cost by up to 60%. The gains come primarily from improved context management rather than longer reasoning traces — exactly what `system_prompt_extra` controls.

This means `--optimize` yields disproportionate gains on local models (Qwen, LLaMA, DeepSeek) compared to cloud models (Claude, GPT) that already have strong instruction-following. For a 27B-35B model on consumer hardware, even `--optimize light` (3 rounds) can meaningfully close the gap with cloud-tier performance.

**Recommended onboarding workflow:**

```bash
# 1. Create a model profile with conservative defaults
cat > ~/.nash/models/qwen3-30b.toml << 'EOF'
match = "qwen3-30b"
chars_per_token = 4.0

[thinking]
mode = "on"
budget = 8192

[client]
temperature = 0.5
max_tokens = 12288

[react]
max_react_steps = 30
cycling_detection = true

system_prompt_extra = ""
EOF

# 2. Run baseline regression to see where you start
nash --regression

# 3. Run optimization (light is usually sufficient)
nash --optimize light

# 4. Verify the optimized prompt actually improved things
nash --validate-harness baseline                # save current as baseline
nash --regression                               # run with optimized prompt
nash --validate-harness compare                 # compare against baseline

# 5. Inspect the resulting spec
nash --spec | grep -A 20 system_prompt_extra
```

The optimizer writes accepted prompt edits directly into the model profile's `system_prompt_extra` field. Each accepted edit is non-regressive by construction — held-in score never decreases, and at least one split improves.

#### When to Re-optimize

Re-run `--optimize` when:
- **Upgrading a model** — a new Qwen or LLaMA release may have different failure modes
- **Changing the task domain** — switching from coding to research tasks may need different prompting
- **After adding new tools** — the model may need guidance on when/how to use them
- **After significant config changes** — new memory thresholds, eviction policies, or tool restrictions

### Error Recovery

#### HTTP 500 — 3-Tier Retry Strategy

When the LLM server returns HTTP 500 (malformed tool_calls JSON, server crash):

| Tier | Attempt | Strategy | Rationale |
|------|---------|----------|-----------|
| 1 | 2nd | Remove last assistant+tool_result pair | Model's last output was malformed |
| 2 | 3rd | Reformulate scratchpad via LLM | Code blocks in scratchpad confuse JSON generation |
| 3 | 4th | Strip scratchpad entirely | Nuclear option — remove all context pollution |
| — | — | Give up | All recovery strategies exhausted |

Each tier logs a `server_error` entry to the journal with full diagnostics:
- `server_message` — actual error from the server
- `request_ref` — raw request body stored in store/ (for post-mortem)
- `response_ref` — raw server response stored in store/

Retry count and backoff delay are configurable via `provider_max_retries` and `provider_retry_base` in `[limits]` (defaults: 10 retries, 10s base delay).

#### Unknown Tool Recovery

When the model generates a non-existent tool name (e.g., `shell_execshell_exec`):
1. `tool_execute()` tries longest-prefix match against the dispatch table
2. If a prefix matches, dispatches to that tool automatically
3. Otherwise returns error with available tools list
4. React loop injects corrective user message and lets the model retry

#### Done Result Fallback

When the model puts the summary in `thought` instead of `result` (common with local models):
```c
if ((!result || !result[0]) && thought && thought[0]) {
    result = thought;  // thought IS the answer for done calls
}
```

### file_read with Line Ranges

Nash's `file_read` tool supports `start_line` and `end_line` parameters to eliminate the need for `shell_exec sed/head/tail` hacks:

```json
{"path": "src/react.c", "start_line": 100, "end_line": 200}
```

- **1-based indexing** — matches editor line numbers
- **Negative start_line** — `start_line: -20` reads last 20 lines (tail behavior)
- **Line numbers in output** — each line prefixed with its number (`100: static void ...`)
- **total_lines in response** — helps model decide whether to use ranges on next call
- **Backward compatible** — no parameters = full file read

### Web Search with SearXNG Auto-Start

The `web_search` tool uses **SearXNG** as its search backend (self-hosted, private).

When SearXNG is configured but not running, nash **automatically starts a SearXNG container** using podman (preferred) or docker:

1. Creates a persistent config directory at `~/.nash/searxng/` with a `settings.yml` enabling JSON API
2. Launches `docker.io/searxng/searxng:latest` with bind-mounted config
3. Waits for the container to respond
4. Tears down the container on nash exit (`web_search_cleanup()`)

The bundled `config/searxng/settings.yml` provides a minimal override that inherits SearXNG defaults while enabling JSON output format and configuring search engines for coding tasks.

### Interactive user_ask Tool

The `user_ask` tool allows the LLM to pause inference and ask the user a clarifying question:

1. The inference thread sets `user_ask_pending` and emits a `REACT_EVENT_USER_ASK` event
2. The TUI displays the question and waits for user input
3. The user's response is passed back to the inference thread via `user_ask_answer`
4. The react loop resumes with the answer injected into context

This enables the agent to resolve ambiguities rather than guessing, particularly useful for tasks with underspecified requirements.

### Playbook Run Logs

Every playbook execution is logged to `~/.nash/runs/` as a JSONL file with events:
- `start` — playbook name, number of passes
- `pass` — pass index, label, session ID
- `done` — pass completion status
- `end` — overall result (ok/fail)

View run history with `/runs` (list) and `/runs show ID` (details) in the TUI.

### Unified Spec — Reproducible Configuration Snapshots

Inspired by [OpenJarvis](https://arxiv.org/abs/2605.17172) (Stanford, 2026), which formalizes personal AI systems as a composition of five typed primitives (Intelligence, Engine, Agents, Tools & Memory, Learning) bundled into a single versioned "spec," nash implements **full spec serialization and import**.

OpenJarvis showed that when you swap a cloud model for a local one, accuracy drops 25–39 pp because the entire stack was co-designed for the cloud model. Prompt-only tuning recovers just ~5 pp. The solution: jointly optimize across all primitives via a typed spec that captures the *complete* configuration. Nash's layered model profiles implement this — the profile carries the full "tuned configuration around the model," and `--spec` makes it inspectable.

#### `nash --spec` — Export

Dumps the fully-resolved configuration after all layers are applied (compile defaults → `config.toml` → model profile → CLI flags) as a single TOML document:

```bash
$ nash --spec
# Nash Spec (fully resolved)
# Model: claude-opus-4-6 via vertex
# Profile: claude.toml
# Generated: 2026-06-12T09:02:29Z

[provider]
type = "vertex"
model_id = "claude-opus-4-6"
context_size = 1000000
chars_per_token = 3.5

[client]
temperature = 0.7
max_tokens = 16384

[thinking]
mode = "yes"
budget = -1

[react]
max_react_steps = 0
inject_memory = true
enable_reflection = true
enable_pruning = true
...

[memory]
recall_min_score = 0.15
vscore_exponent = 0.3
...

[tools]
active = ["shell_exec", "file_read", ...]

[embedding]
type = "onnx"

[limits]
...

[memory_belief_entropy]
enabled = false
...
```

This enables:
- **Reproducibility** — capture exactly what configuration produced a result
- **A/B comparison** — `nash --spec > before.toml`, make changes, `nash --spec > after.toml`, diff
- **Spec sharing** — share optimized configs ("here's my coding spec for Qwen3-30B on an M4 Mac")

#### `nash --load-spec FILE` — Import

Loads a TOML spec file as a configuration overlay on top of the existing config. Only specified sections are applied; everything else keeps its current value:

```bash
# Round-trip: export → modify → reimport
nash --spec > my-spec.toml
vim my-spec.toml                   # adjust parameters
nash --load-spec my-spec.toml      # apply as overlay

# Verify round-trip fidelity
nash --spec > a.toml
nash --load-spec a.toml --spec > b.toml
diff a.toml b.toml                 # identical
```

Supported overlay sections: `[provider]`, `[client]`, `[thinking]`, `[react]` (flags + limits), `[memory]`, `[tools]` (allow/block/description overrides), `[limits]`, `[memory_belief_entropy]`.

#### Mapping to OpenJarvis Primitives

| OpenJarvis Primitive | Nash Spec Section | Implementation |
|---------------------|-------------------|----------------|
| **Intelligence** | `[provider]`, `[thinking]`, model profiles | `provider.c` vtable + `config_match_model()` |
| **Engine** | `[client]`, `[limits]`, `[embedding]` | `config.c` + `embedding.c` |
| **Agents** | `[react]` (flags + limits) | `react.c` + `react_flags_t` |
| **Tools & Memory** | `[tools]`, `[memory]`, `[memory_belief_entropy]` | `tools.c` + `memory.c` |
| **Learning** | Playbooks, regression, postmortem | `playbook.c` + `regression.c` + `postmortem.c` |

---

## Configuration

Nash uses TOML configuration at `~/.nash/config.toml`:

```toml
[server]
api_base = "http://192.168.1.18:8080"    # llama.cpp server
# api_base = "http://localhost:11434"     # Ollama

[provider]
# type = "openai"                         # local | openai | anthropic | vertex
# model_id = "gpt-4o"
# api_key_env = "OPENAI_API_KEY"
# project_id = "my-gcp-project"          # Vertex AI project
# region = "us-east5"                     # Vertex AI region
# context_size = 200000                   # context window (0 = auto)
# chars_per_token = 3.5                   # chars per token ratio
# caching = false                         # prompt caching (Anthropic)

[thinking]
mode = "on"                               # off | on | structural
budget = -1                               # -1=unrestricted, 0=none, N>0=max tokens

[embedding]
type = "onnx"                             # onnx | ollama | openai | none
model_path = "~/models/all-MiniLM-L6-v2"  # ONNX model directory

[limits]
shell_timeout = 300                       # seconds
shell_max_output = 512000                 # bytes (~512KB)
file_max_size = 52428800                  # 50MB
max_react_steps = 0                       # 0 = unlimited
llm_timeout = 300                         # seconds per LLM call
provider_max_retries = 10                 # max HTTP retries for LLM provider
provider_retry_base = 10                  # base delay (seconds) between retries

[memory]
recall_min_score = 0.15                   # normalized [0, 1] threshold
vscore_exponent = 0.3                     # power-law exponent for validation score
memory_index_max = 50                     # max entries in memory index
max_skills_per_query = 2                  # skills loaded per query
max_lessons_per_query = 2                 # lessons loaded per query
max_strategies_per_query = 1              # strategies loaded per query
max_antipatterns_per_query = 1            # anti-patterns loaded per query
max_recalled_per_query = 8                # unified recall limit (L4 + L3 combined)
prune_min_score = 0.35                    # Bayesian pruning threshold
prune_min_evidence = 3                    # min recalls before pruning
consolidation_threshold = 0.82            # cosine threshold for dedup
recall_blend_semantic = 0.4               # semantic similarity weight (grep-favoring)
recall_blend_substring = 0.6              # substring match weight (grep-favoring)
dream_reminder_threshold = 50            # new entries before dream reminder

# Error-triggered reactive retrieval
error_recall_min_length = 10
error_recall_candidates = 3
error_recall_max_inject = 1
error_recall_min_relevance = 0.25

# Eviction-triggered re-retrieval
eviction_recall_candidates = 3            # candidates to consider post-eviction
eviction_recall_min_relevance = 0.30      # min relevance for post-eviction injection

# Cycling-triggered retrieval
cycling_recall_candidates = 2             # candidates when agent is cycling
cycling_recall_min_relevance = 0.30       # min relevance for cycling injection

# Temporal event calendar
temporal_calendar = true                  # inject [TEMPORAL CONTEXT] section
temporal_recent_days = 7                  # "Recent" window in days
temporal_older_days = 30                  # "Older" window in days
temporal_max_entries = 20                 # max entries in calendar

# Episodic recall from past sessions
episodic_recall = true                    # query session_index at task start
episodic_max_results = 2                  # max session chunks to inject
episodic_min_score = 0.35                 # min similarity for injection

# Associative graph walk
associative_depth = 1                     # follow refs[] depth (0 = disabled)

# Working memory auto-promotion
auto_promote = true                       # auto-save findings to scratchpad
auto_promote_min_length = 500             # min tool output length to trigger
auto_promote_max_chars = 2000             # max auto_findings section size

[context]
context_eviction_pct = 70                 # evict when context > 70% full

[workspace]
# active = "work-acme"                    # current workspace (empty = global-only)
# global_recall = true                    # also search global memory during recall
# global_recall_weight = 0.8              # score multiplier for global results
# isolated = false                        # fully isolated — no global memory access

[session]
# max_indexed_sessions = 0               # max sessions in memory index (0 = no limit)

[search]
engine = "searxng"                        # SearXNG (auto-started via podman/docker)
# searxng_url = "http://localhost:8080"   # custom SearXNG instance
```

---

## Building

### Dependencies

- **C11 compiler** (gcc or clang)
- **libcurl** — HTTP client for LLM API calls
- **OpenSSL** (libcrypto) — SHA-256 for content-addressed store
- **readline** — command-line input
- **ncursesw** — TUI rendering (wide-char support)
- **pthreads** — concurrent inference and TUI threads
- **ONNX Runtime** (optional) — local embedding inference
- **podman or docker** (optional) — SearXNG container auto-start for `web_search`

### Build

```bash
cd nash
make
```

### Run

```bash
# Interactive TUI mode
./nash

# Single query mode (headless)
./nash -p "fix the memory leak in tools.c"

# With custom API endpoint
./nash --api http://localhost:8080

# Run a playbook (e.g., memory consolidation)
./nash --play dream

# Resume an existing session
./nash --session ~/.nash/sessions/1779970830.40871

# Custom data directory
./nash --data-dir /path/to/nash-data

# Dump fully-resolved spec (all config layers applied)
./nash --spec

# Load a spec overlay and run
./nash --load-spec my-tuned-spec.toml -p "fix the bug"

# Named workspace (memories isolated from other workspaces)
./nash -w work-acme                   # or --workspace work-acme

# Fully isolated workspace (no global memory recall)
./nash -w personal --isolated
```

### Self-Harness Commands

```bash
# Run regression test suite
./nash --regression
./nash --regression --split held-in

# Validate harness changes
./nash --validate-harness baseline
./nash --validate-harness compare

# Postmortem failure analysis
./nash --postmortem
./nash --postmortem-sessions 100
```

### Testing

```bash
make test    # runs unit tests: test_memory, test_store, test_config, test_str, test_journal, test_memory_context, test_spec

# Integration tests (requires a running LLM server)
./tests/run_integration.sh --api http://localhost:8080
```

---

## Session Structure

```
~/.nash/
├── config.toml              # Configuration
├── memory/                  # Persistent memory — GLOBAL layer (git-backed)
│   ├── lesson:*.json        # Lessons learned
│   ├── strategy:*.json      # Reusable procedures
│   ├── skill:*.json         # Domain knowledge
│   ├── fact:*.json          # Concrete data
│   ├── task:*.json          # Ongoing work
│   ├── anti-pattern:*.json  # What not to do
│   ├── .last_dream          # Timestamp of last dream consolidation
│   └── .git/                # Full history
├── workspaces/              # Per-workspace memory isolation
│   ├── work-acme/
│   │   └── memory/          # WORKSPACE layer (work-only, git-backed)
│   └── personal/
│       └── memory/          # WORKSPACE layer (personal-only)
├── models/                  # Per-model profiles
│   └── *.toml               # e.g., qwen3.toml, claude.toml
├── playbooks/               # Custom playbooks (dream.yaml auto-seeded)
│   ├── dream.yaml
│   ├── reflect.yaml
│   ├── digest.yaml
│   ├── health.yaml
│   ├── prune.yaml
│   ├── retrospect.yaml
│   └── self-harness.yaml
├── regression/              # Regression test query banks
│   └── *.yaml               # held-in / held-out query banks
├── runs/                    # Playbook run logs (JSONL)
│   └── *.jsonl              # e.g., 1779970830.dream.jsonl
├── searxng/                 # SearXNG container config (auto-created)
│   └── settings.yml         # Persistent search engine settings
├── store/                   # Content-addressed artifacts (SHA-256)
│   ├── a1b2c3d4...          # Tool outputs, errors, etc.
│   └── ...
├── postmortem.md            # Latest failure analysis report
└── sessions/
    ├── my-project → 1779970830.40871  # Named session symlink
    └── 1779970830.40871/    # Session directory
        ├── journal.jsonl    # Full event log
        ├── scratchpad.jsonl # Persistent working notes (JSONL, append-only)
        ├── scratchpad.md    # Legacy format (loaded as fallback)
        ├── summary.txt      # Session manifest (journal_manifest() output)
        ├── summary.emb      # Embedding vector for semantic search
        ├── checkpoint.json  # Resume state (if interrupted)
        ├── R0S0 → ../../store/...  # Step aliases (symlinks)
        ├── R0S1 → ../../store/...
        └── ...
```

---

## Research Foundations

Nash's design is grounded in recent research on agentic memory systems, cognitive architectures, and LLM reasoning:

### Memory Architecture
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [Generative Agents](https://arxiv.org/abs/2304.03442) | 2023 | Composite scoring (recency × importance × relevance) | Hybrid scoring with semantic + substring + Bayesian validation |
| [Memory Survey](https://arxiv.org/abs/2404.13501) | 2024 | Five critical memory operations including validation | Bayesian validation scoring (hits/misses) |
| [CALMem](https://arxiv.org/abs/2605.20724) | 2026 | Token-budget-adaptive injection (MOIM) | Inspired budget-aware memory injection design |
| [Mem-π](https://arxiv.org/abs/2605.21463) | 2026 | Generative memory policy, learned abstention +59% | Query-time synthesis with semantic abstention ("NONE") |
| [DeferMem](https://arxiv.org/abs/2605.22411) | 2026 | Query-time evidence distillation | Memory synthesis produces faithful, self-contained guidance |
| [MemForest](https://arxiv.org/abs/2605.23986) | 2026 | Temporal indexing, memory relevance changes over time | Inspired temporal relevance awareness in scoring design |
| [MemFail](https://arxiv.org/abs/2605.26667) | 2026 | Weak memory injection hurts performance | Bayesian scoring + abstention gate filters low-quality memories |
| [MemMorph](https://arxiv.org/abs/2605.26154) | 2026 | Raw storage insufficient, needs active management | Post-loop pruning + consolidation |
| [ByteRover](https://arxiv.org/abs/2604.01599) | 2026 | Agent-native hierarchical memory with zero external infrastructure; LLM curates its own Context Tree | Episodic recall from session index — journals as agent-native episodic memory with no vector DB |
| [CogniFold](https://arxiv.org/abs/2605.13438) | 2026 | Always-on proactive memory via cognitive folding; extends CLS theory to 3 layers with graph self-organization | Working memory auto-promotion — harness auto-saves findings to scratchpad without explicit agent action |
| [MemCog](https://arxiv.org/abs/2605.28046) | 2026 | Memory-as-Cognition: navigable memory store with associative link graphs and proactive reasoning protocol; SOTA on LoCoMo (92.98) and LongMemEval (95.8) | Memory-as-Cognition principle — harness controls all retrieval timing; associative graph walk follows refs[] on recalled memories |
| [MRAgent](https://arxiv.org/abs/2606.06036) | 2026 | Memory is reconstructed, not retrieved: associative Cue-Tag-Content graph with active reconstruction; +23% on LoCoMo/LongMemEval (ICML 2026) | Associative graph walk: depth-1 ref following injects referenced memories during recall |
| [MemRefine](https://arxiv.org/abs/2606.13177) | 2026 | LLM-guided compression for budget-constrained long-term memory; similarity-based candidate pairs with delete/merge/preserve decisions | Informed design of memory pruning: aggressive dead-weight removal (73% never-recalled entries deleted) |

### Cognitive Architecture
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [OpenDev / Terminal AI Agents](https://arxiv.org/abs/2603.05344) | 2026 | Structural thinking separation — remove tools during reasoning phase | Structural reasoning mode (default): two-call pattern (reason without tools → act with tools) |
| [TriMem](https://arxiv.org/abs/2605.19952) | 2026 | Three-tier memory (working/episodic/semantic) | Scratchpad (working) + journal (episodic) + memory (semantic) |
| ["Language Models Need Sleep"](https://arxiv.org/abs/2605.26099) | 2026 | Dreaming/consolidation essential for memory health | Post-loop Bayesian pruning + dedup + consolidation |
| [MMPO](https://arxiv.org/abs/2605.30159) | 2026 | Belief Entropy ℋ_BE measures memory clarity | Belief Entropy monitoring for memory quality signal |
| [Harness-1](https://arxiv.org/abs/2606.02373) | 2026 | Stateful cognitive offloading — move bookkeeping from LLM to environment-side harness | Importance-tagged messages, multi-pass progressive eviction, sentence-BM25 compression, CRC32 context dedup, auto-seeding scratchpad, tool diversity nudge |

### Skill Extraction
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [CODESKILL](https://arxiv.org/abs/2605.25430) | 2026 | RL-trained skill extraction from completions | Post-task reflection extracts reusable lessons/strategies |
| [MUSE-Autoskill](https://arxiv.org/abs/2605.27366) | 2026 | Self-evolving skill library | Skills recalled semantically per query, refined via validation |

### Self-Improvement & Spec Optimization
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [OpenJarvis](https://arxiv.org/abs/2605.17172) | 2026 | Personal AI = 5 typed primitives (Intelligence, Engine, Agents, Tools, Learning) in a jointly-optimizable spec. LLM-guided spec search across all primitives recovers cloud-level accuracy on-device. | Unified spec (`--spec` / `--load-spec`), layered model profiles with 30+ override fields, sentinel-based cascade (Model Profiles, Unified Spec sections) |
| [Self-Harness](https://arxiv.org/abs/2606.09498) | 2026 | Weakness mining + proposal + validation gate | Postmortem analysis + regression testing + validation gate |
| [DCPM](https://arxiv.org/abs/2606.09483) | 2026 | Dual-process cognitive memory with async consolidation | Auto-dream: usage-based memory consolidation trigger |
| [SWE-Shepherd](https://arxiv.org/abs/2604.10493) | 2026 | Process Reward Models (PRMs) for step-level supervision in code agents | Step-level trajectory scoring in postmortem: productive/wasteful/harmful/spinning classification per tool call, causal step attribution for failures |
| [EvolveMem](https://arxiv.org/abs/2605.13941) | 2026 | Self-evolving memory architecture — expose retrieval config as structured action space optimized by LLM diagnosis | Memory quality telemetry (journal `memory_quality` entries), self-harness retrieval diagnosis pass, data-driven tuning of retrieval params |
| [RHI](https://arxiv.org/abs/2607.15524) | 2026 | Recursive Harness Self-Improvement — harnesses are data-generating components; pairwise feedback over revision history; gains from context management outweigh longer reasoning | `--optimize` as standard local-model onboarding step; validates prompt-level harness optimization yields disproportionate gains on low-reasoning-effort models |

### Context Management
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [CWL — Context Window Lifecycle](https://arxiv.org/abs/2606.11213) | 2026 | Typed, dependency-linked episodes; deterministic LLM-free eviction based on recoverability | Recoverability-aware eviction: messages annotated with `RECOVER_NONE/SCRATCHPAD/STORE/FILE/MEMORY`, sorted by recoverability during Pass 3 eviction |
| [LCM — Lossless Context Management](https://arxiv.org/abs/2605.04050) | 2026 | Recursive context compression via hierarchical summary DAG with lossless pointers | LCM-Lite: breadcrumb index of evicted store refs injected at eviction point, making eviction lossless via `file_read` recovery |

### Agentic Search & Retrieval
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [Chronos](https://arxiv.org/abs/2603.16862) | 2026 | Agentic harness evaluation framework; temporal event structuring as most impactful component; harness ≈ retriever in impact on accuracy | Temporal event calendar; event-driven architecture validated by harness-vs-retriever finding |
| ["Is Grep All You Need?"](https://arxiv.org/abs/2605.15184) | 2026 | Grep beats vector for inline delivery; file-based delivery inverts rankings; temporal events most impactful single component; rendering = retrieval + orchestration | 40/60 semantic/substring blend (grep-favoring); inline-only memory delivery; temporal calendar; enriched rendering with recency/confidence metadata |
| [Retrieval Timing Bottleneck](https://arxiv.org/abs/2605.30621) | 2026 | Retrieval timing, not storage quality, is the bottleneck in memory-augmented agents; single retrieval at task start creates timing mismatch as agent needs evolve | Event-driven re-retrieval: eviction-triggered, cycling-triggered, error-triggered — all using event content as the retrieval query |
| [Recursive Agent Harnesses](https://arxiv.org/abs/2606.13643) | 2026 | Parent agents spawn sub-agent harnesses; harness recursion improves Codex from 71.75% → 81.36% on Oolong-Synthetic | Validates harness-investment approach — orchestration matters more than model capability |
| [Ask Early, Ask Late, Ask Right](https://arxiv.org/abs/2605.07937) | 2026 | Clarification timing matters: goal clarification loses value after 10% execution; no frontier model asks within optimal window | `user_ask` tool with system prompt guidance to ask early when uncertainty ≥ 0.5 |
| [MemMachine](https://arxiv.org/abs/2604.04853) | 2026 | Ground-truth-preserving memory combining short-term, long-term episodic, and profile memory | Episodic recall: raw journal chunks preserve ground-truth tool sequences alongside distilled L4 memories |

### Additional References
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [ActiveGraph](https://arxiv.org/abs/2605.21997) | 2026 | Typed edges between memory nodes | Memory tagging and cross-reference system |
| [MemIR](https://arxiv.org/abs/2605.25869) | 2026 | Provenance chains linking raw evidence | Journal + store provide full provenance for every artifact |

---

## License

MIT

---

## Contributing

Nash is a personal project focused on exploring what's possible with local LLMs as autonomous coding agents. The codebase is intentionally compact and self-contained.

Key design principles:
- **No Python dependencies** — single compiled binary
- **Local-first** — works with llama.cpp, no cloud required
- **Research-grounded** — every major design decision cites its research basis
- **Self-improving** — the agent learns from every task via persistent memory
- **Full audit trail** — every action is stored, referenced, and inspectable
