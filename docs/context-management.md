# Context Management

## Structural Reasoning (Default)

Nash's default thinking mode is **structural reasoning** -- a two-call pattern inspired by [arXiv:2603.05344] that separates reasoning from action:

1. **Call 1: Reason** -- LLM is called with **no tool schemas** and thinking OFF. Without tools available, the model is structurally forced to produce plain-text analysis instead of jumping to action.
2. **Call 2: Act** -- Tools are restored. The reasoning from Call 1 persists as an assistant message in context, informing the tool call.

Key advantages over native thinking (`on` mode):
- **Persistent reasoning** -- native thinking vanishes after each turn; structural reasoning becomes a regular context message visible to all future steps
- **Provider-agnostic** -- works with every provider (local, OpenAI, Anthropic, Vertex) with no API requirements
- **Faster & cheaper** -- models produce concise analysis (500-1500 tokens) vs verbose native thinking (2000-8000 tokens)
- **Controllable** -- reasoning depth is shaped by the prompt, not a token budget knob
- **Smaller request payload** -- Call 1 has no tool schemas (~2-4K fewer input tokens)

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

## Harness-1 Context Management

Inspired by [Harness-1](https://arxiv.org/abs/2606.02373) (UIUC/Berkeley/Chroma, 2026), nash implements **stateful cognitive offloading** -- moving context bookkeeping from the LLM to the environment. Instead of letting the model waste reasoning capacity on tracking what it has seen, managing duplicates, and deciding what to keep, the harness handles this automatically.

Six mechanisms adapted from the paper:

### 1. Importance-Tagged Messages

Every message in the LLM context carries an importance level (`CRITICAL` / `HIGH` / `NORMAL` / `LOW`) auto-assigned by type:

| Importance | Message Types |
|------------|---------------|
| **CRITICAL** | System prompt, user query, scratchpad, `done` results |
| **HIGH** | `grep_search`, `memory_search`, recent assistant turns |
| **NORMAL** | `file_read`, `shell_exec`, `web_fetch` |
| **LOW** | Errors, hints, deduplicated results |

Importance controls eviction priority -- `LOW` messages are stripped first, `CRITICAL` messages are never evicted.

### 2. Multi-Pass Progressive Context Rendering

Replaces the previous single-pass binary eviction with 3-pass progressive degradation:

| Pass | Trigger | Action |
|------|---------|--------|
| **Pass 1** | Context > threshold | Strip `LOW` importance messages (errors, hints, deduped) |
| **Pass 2** | Still over threshold | Compress `NORMAL` messages to top-4 sentences via sentence-BM25 |
| **Pass 3** | Still over threshold | Standard eviction with scratchpad extraction + compaction floor |

This preserves more useful context at each stage instead of dropping messages wholesale.

### 3. Sentence-BM25 Relevance Compression

New `compress.c` module (301 lines) implements relevance-based text compression:

- Splits text into sentences
- Filters stopwords (70 English + 20 programming terms) to prevent common words from drowning out semantically meaningful query terms
- Scores each sentence by BM25-like term overlap with the current query
- Keeps top-N sentences in their original order

Used in eviction Pass 2 instead of blind character-limit truncation. A `file_read` output that returned 200 lines gets compressed to the 4 most relevant sentences rather than keeping the first N characters.

### 4. Context-Level Deduplication

CRC32 hash tracking in a 64-entry rolling buffer detects when the same content appears multiple times in context (e.g., reading the same file twice, overlapping `grep_search` results):

```
[dedup] Same as step 3 -- see earlier result
```

Prevents wasting context on re-reading identical content.

### 5. Auto-Seeding Scratchpad

The first successful tool result (step 0) automatically seeds the scratchpad at priority 3, ensuring it's never empty when eviction kicks in. This implements Harness-1's "warm-started curation" -- the agent starts with something rather than a cold empty state.

### 6. Tool Diversity Nudge

Tool usage tracking (`tool_use_counts[32]` in `tool_ctx_t`) monitors which tools the agent is using. After 10+ steps without calling `notes()` to save findings, a one-shot soft hint is injected:

> *Consider saving key findings to your scratchpad with notes() -- they survive context eviction.*

This prevents the common failure mode of accumulating context without ever externalizing key findings.

**Files**: `src/llm.h`, `src/llm.c`, `src/tools.h`, `src/react.c`, `src/compress.c`, `src/compress.h`

## Scratchpad-Only Architecture (v5)

Nash uses a **scratchpad-only** architecture for cross-loop state management. Each react loop starts with a fresh context containing only:

```
[system]  System prompt (rules, tool definitions)
[user]    Memory (index + pinned + relevant skills)
[user]    Scratchpad (persistent working notes)
[user]    User query
```

No manifest. No last-exchange injection. No truncation. The scratchpad is the **sole** mechanism for passing state between react loops.

### JSONL-Backed Persistence (L2)

Scratchpad sections are persisted to `scratchpad.jsonl` -- an append-only JSONL file using the same convention as `journal.jsonl`. Each line is a self-contained JSON object; **last entry per section name wins**:

```jsonl
{"name":"findings","priority":1,"content":"The root cause is a null pointer..."}
{"name":"plan","priority":3,"content":"1. Add null check\n2. Add test\n3. Run suite"}
{"name":"findings","priority":1,"content":"Updated: confirmed null deref at line 73..."}
{"name":"status","op":"clear"}
```

This replaces the previous `<!-- priority:X -->` HTML comment format, which was fragile (invisible to LLMs, order-dependent parsing, round-trip loss). Benefits:

- **Metadata separation** -- priority lives in JSONL fields, never in content. The LLM sees clean markdown; parsing is trivial `cJSON_Parse()` per line.
- **Crash safety** -- append-only writes mean a crash mid-save loses at most the last partial line. All previously written sections survive intact.
- **History for free** -- the JSONL file records every section update in order, useful for debugging or replaying scratchpad evolution.
- **Clean round-trip** -- the LLM sees clean markdown without priority markers. When pruning output is parsed back, section names match and priorities are preserved from JSONL metadata.
- **Legacy fallback** -- existing `scratchpad.md` files load via the legacy parser. New saves always use JSONL. Gradual migration with no flag day.

### Context Eviction -- Recoverability-Aware + Lossless Breadcrumbs

When context usage exceeds the eviction threshold (default 70%), nash uses a **multi-pass progressive eviction** pipeline inspired by two research papers:

**CWL (Context Window Lifecycle)** [arXiv:2606.11213]: Messages are annotated with a **recoverability level** based on their tool type:

| Recoverability | Tools | Eviction Priority |
|---------------|-------|-------------------|
| `RECOVER_FILE` | file_write, file_edit | Evict first (content persisted in filesystem) |
| `RECOVER_MEMORY` | memory_store, memory_pin | Evict first (content in long-term memory) |
| `RECOVER_SCRATCHPAD` | notes, plan | Evict early (content in scratchpad) |
| `RECOVER_STORE` | Any tool with store ref | Evict early (recoverable via `file_read`) |
| `RECOVER_NONE` | grep_search, web_fetch, etc. | Evict last (observations not persisted) |

During Pass 3 eviction, messages are sorted by `(importance ASC, recoverability DESC, age ASC)` -- recoverable messages are evicted before non-recoverable ones at the same importance level.

**LCM-Lite (Lossless Context Management)** [arXiv:2605.04050]: Instead of losing evicted content, nash generates a **breadcrumb index** of evicted store refs:

```
[EVICTED CONTEXT -- recoverable via file_read]
- R0S15: {"pattern":"eviction","path":"src/","matches":50... (user, 837 chars)
- R0S22: {"path":"src/memory.c","total_lines":1932... (user, 9915 chars)
```

The breadcrumb is injected after the scratchpad at the eviction point, giving the LLM awareness of what was evicted and how to recover it. Non-recoverable messages still get heuristic extraction into the scratchpad.

This replaces the old approach of uniform scratchpad dumping with a two-track system: recoverable content gets indexed, non-recoverable content gets summarized.

### Post-Done Scratchpad Pruning

After each task completes, an LLM call removes resolved/completed information from the scratchpad:

> *"Remove ONLY information from the scratchpad that was resolved or completed by this task. Keep everything else exactly as-is -- do not rewrite, merge, summarize, or reformat."*

The `extract_llm_text_output()` helper accepts both plain markdown and JSON tool-call format from the LLM, extracting the `content` field from JSON if the model outputs a tool call instead of plain text.

### Auto-Save Done Results

When `done` is called, the result is automatically saved to the scratchpad as `R<N>_result` (priority 1), ensuring the next react loop has full access to the previous loop's conclusion.
