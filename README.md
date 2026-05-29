# nash — Autonomous Coding Agent in C

**nash** is a fully autonomous coding agent implemented in ~34,000 lines of C. It connects to any OpenAI-compatible LLM server (llama.cpp, OpenAI, Anthropic, Vertex AI) and executes multi-step coding tasks through a ReAct (Reason + Act) loop with persistent memory, a TUI interface, and research-grounded cognitive architecture.

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
│ local    │ semantic │ 16 tools  │ content-addressed     │
│ openai   │ Bayesian │ registry  │ full audit trail      │
│ anthropic│ pruning  │ dispatch  │ checkpoint/resume     │
│ vertex   │ pinning  │           │                       │
├──────────┴──────────┴───────────┴───────────────────────┤
│              LLM Server (llama.cpp / API)                │
└─────────────────────────────────────────────────────────┘
```

---

## Features

### 1. ReAct Loop with Native Tool Calling

Nash implements a full ReAct (Reason + Act) loop that drives autonomous task completion:

```
User Query → [Plan] → Tool Call → Observe Result → [Reflect] → Next Tool Call → ... → Done
```

- **Native OpenAI tool_calls API** — uses structured `tool_calls` with `tool_call_id` threading, not JSON-in-content hacks
- **16 built-in tools** — shell_exec, file_read, file_write, file_edit, grep_search, glob_search, web_fetch, web_search, notes, plan, done, memory_store, memory_recall, memory_pin, memory_unpin, user_ask
- **Shared tool registry** (`tools_registry.h`) — tool definitions defined once, formatted per-provider (local/OpenAI/Anthropic)
- **Dispatch table** — tool execution via function pointer table, not strcmp chains
- **Cycling detection** — detects repeated identical tool calls, injects corrective guidance, refuses after 4+ repetitions
- **Unknown tool recovery** — when the model generates a garbled tool name (e.g., `shell_execshell_exec`), injects a corrective message listing available tools and lets the model retry

### 2. Multi-Provider Support

Nash supports four LLM providers through a unified vtable interface:

| Provider | Endpoint | Features |
|----------|----------|----------|
| **Local** (llama.cpp) | Any OpenAI-compatible server | `chat_template_kwargs`, `reasoning_budget`, EDRM probing |
| **OpenAI** | `api.openai.com` | Strict mode (`additionalProperties: false`), prompt caching |
| **Anthropic** | `api.anthropic.com` | `input_schema` format, extended thinking, prompt caching |
| **Vertex AI** | Google Cloud | OAuth2 token management, Anthropic-on-Vertex |

All providers share the same tool registry and SSE streaming infrastructure. Provider-specific differences (JSON structure, auth headers, error formats) are encapsulated in the vtable.

### 3. Persistent Memory System

Nash maintains a persistent, git-backed memory system that survives across sessions. Memories are categorized as **lessons** (what went wrong/right), **strategies** (reusable procedures), **skills** (domain-specific knowledge), and **anti-patterns** (what not to do).

#### Hybrid Scoring — Semantic + Substring + Bayesian Validation

Memory recall uses a composite scoring function that blends three signals:

```
relevance = semantic_similarity * 0.7 + substring_match * 0.3   (normalized to [0, 1])
importance = log(1 + access_count) / 5.0                        (normalized to [0, 1])
composite = relevance * 0.8 + importance * 0.2
final_score = composite * vscore
```

Where `vscore` is a **Bayesian validation score** using Beta posterior mean with Laplace smoothing:

```
vscore = (recall_hits + 1) / (recall_hits + recall_misses + 2)
```

New memories start at vscore=0.5 (maximum entropy). Memories that consistently correlate with task failures get demoted. This is inspired by:

- **MemFail** [arXiv:2605.26667] — diagnostic benchmark showing that injecting weakly-relevant memories *hurts* performance. Bayesian scoring provides the data-driven signal to identify which memories are genuinely useful.
- **Generative Agents** [Park et al., 2023] — composite scoring (recency × importance × relevance) as the foundation for memory retrieval ranking.
- **Memory Survey** [arXiv:2404.13501] — comprehensive survey identifying five critical memory operations, including validation/reflection as essential for memory quality.

#### Semantic Embeddings

When configured, nash uses dense vector embeddings for semantic similarity:

- **ONNX Runtime** — local inference with models like `all-MiniLM-L6-v2` (no API calls needed)
- **Ollama** — embedding via local Ollama server
- **OpenAI** — embedding via OpenAI API

Cosine similarity is clamped to [0, 1] (negative = no match) and scaled to [0, 6] before blending with substring scores. Without embeddings, pure substring matching is used with a 1.0x scale factor to produce comparable score ranges.

#### Memory Refresh — Adaptive Re-evaluation

Every 3 steps during a react loop, nash re-evaluates memory relevance based on the evolving task context:

> *"A query that starts as 'fix this bug' might evolve into 'redesign the database schema' by step 15. Without re-evaluation, stale memories from step 0 persist."*

This is inspired by:
- **CALMem** [arXiv:2605.20724] — token-budget-adaptive injection mechanism (MOIM) that re-evaluates per turn
- **MemForest** [arXiv:2605.23986] — temporal indexing shows that memory relevance changes over time

#### Memory Synthesis — Query-Time Guidance Generation

When `memory_synthesis` is enabled, retrieved memories are passed through an LLM synthesis call to generate context-adapted guidance instead of injecting verbatim entries:

- **Mem-π** [arXiv:2605.21463] — generative memory policy that generates context-specific guidance, +59% on WebArena
- **DeferMem** [arXiv:2605.22411] — query-time evidence distillation produces faithful, self-contained evidence

The synthesis prompt can respond "NONE" for semantic abstention — more nuanced than score thresholding.

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

### 4. Scratchpad-Only Architecture (v5)

Nash uses a **scratchpad-only** architecture for cross-loop state management. Each react loop starts with a fresh context containing only:

```
[system]  System prompt (rules, tool definitions)
[user]    Memory (index + pinned + relevant skills)
[user]    Scratchpad (persistent working notes)
[user]    User query
```

No manifest. No last-exchange injection. No truncation. The scratchpad is the **sole** mechanism for passing state between react loops.

#### Context Eviction with LLM Summarization

When context usage exceeds the eviction threshold (default 70%), nash uses **LLM-based semantic summarization** instead of destructive deletion:

1. Collect evicted messages into a single text
2. LLM call extracts key findings, decisions, file paths, code changes, errors, conclusions
3. Summary merged into scratchpad as `context_summary` section (priority 0 = highest)
4. Updated scratchpad re-injected at eviction point

This replaces the old approach of deleting messages and re-injecting a navigation manifest (which showed tool calls, not knowledge).

#### Post-Done Scratchpad Pruning

After each task completes, an LLM call removes resolved/completed information from the scratchpad:

> *"Remove ONLY information from the scratchpad that was resolved or completed by this task. Keep everything else exactly as-is — do not rewrite, merge, summarize, or reformat."*

The `extract_llm_text_output()` helper accepts both plain markdown and JSON tool-call format from the LLM, extracting the `content` field from JSON if the model outputs a tool call instead of plain text.

#### Auto-Save Done Results

When `done` is called, the result is automatically saved to the scratchpad as `R<N>_result` (priority 1), ensuring the next react loop has full access to the previous loop's conclusion.

### 5. EDRM — Entropy Dynamics Routing for Thinking Mode

Nash implements **Entropy Dynamics Routing** based on [arXiv:2605.22873] to dynamically decide whether to enable extended thinking (chain-of-thought) for each LLM call:

1. **Probe phase** — generate a short completion (~30 tokens) and analyze entropy dynamics
2. **Compute descriptors** — mean entropy (h_mean), Spearman rank correlation (ρ_s), von Neumann ratio (VNR)
3. **Route decision** — if entropy trajectory shows uncertainty (high h_mean, negative ρ_s, high VNR), enable thinking mode

This avoids the latency and cost of always-on thinking while ensuring complex queries get the reasoning depth they need.

Configuration:
```toml
[thinking]
mode = "edrm"           # off | on | edrm
probe_tokens = 30
probe_n_probs = 10
probe_temperature = 0.6  # probe sampling temperature
tau_rho = -0.1           # Spearman correlation threshold
tau_vnr = 1.5            # von Neumann ratio threshold
tau_h = 4.0              # mean entropy threshold
budget = -1              # -1=unrestricted, 0=none, N>0=max tokens
```

### 6. TUI — Terminal User Interface

Nash provides a full ncurses-based TUI with:

- **Markdown rendering** — headers, bold, italic, inline code, code blocks, tables, horizontal rules, lists
- **Inline formatting in links** — tool names rendered in bold, descriptions as inline code
- **Step expansion** — click/Enter on a step to expand its full content from the store
- **Streaming output** — real-time token display during LLM generation
- **Status bar** — model name, context usage percentage (`ctx 42%`), background jobs count
- **Journal view** — full session history with react loop headers, step markers (+/x), thoughts (💭)
- **Keyboard navigation** — arrow keys, Page Up/Down, Home/End, Enter to expand/collapse

### 7. Content-Addressed Store & Journal

Every tool output, error, and metadata entry is stored in a **content-addressed store** (`~/.nash/store/`) using SHA-256 hashing. Session directories contain symlinks (`R0S0`, `R0S1`, ...) pointing to store entries.

The **journal** (`journal.jsonl`) records every event with:
- React loop number and step
- Tool name and parameters
- Store reference (ref alias)
- Size, line count
- Failed flag and error message
- Tool call ID (for API threading)

All journal entries have store refs — no more `+ ?: memory_context` entries. Every step is viewable and inspectable in the TUI.

### 8. Checkpoint/Resume

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

### 9. Post-Task Reflection

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

### 10. Error Recovery

#### HTTP 500 — 4-Tier Retry Strategy

When the LLM server returns HTTP 500 (malformed tool_calls JSON, server crash):

| Tier | Attempt | Strategy | Rationale |
|------|---------|----------|-----------|
| 1 | 2nd | Remove last assistant+tool_result pair | Model's last output was malformed |
| 2 | 3rd | Reformulate scratchpad via LLM | Code blocks in scratchpad confuse JSON generation |
| 3 | 4th | Strip scratchpad entirely | Nuclear option — remove all context pollution |
| 4 | 5th | Give up | All recovery strategies exhausted |

Each tier logs a `server_error` entry to the journal with full diagnostics:
- `server_message` — actual error from the server
- `request_ref` — raw request body stored in store/ (for post-mortem)
- `response_ref` — raw server response stored in store/

#### Unknown Tool Recovery

When the model generates a non-existent tool name (e.g., `shell_execshell_exec`):
1. `tool_execute()` returns error with available tools list
2. React loop injects corrective user message: "The tool 'X' does not exist. Available tools: ..."
3. Model retries with correct tool name
4. No raw error sent to the model (prevents confusion cascade)

#### Done Result Fallback

When the model puts the summary in `thought` instead of `result` (common with local models):
```c
if ((!result || !result[0]) && thought && thought[0]) {
    result = thought;  // thought IS the answer for done calls
}
```

### 11. file_read with Line Ranges

Nash's `file_read` tool supports `start_line` and `end_line` parameters to eliminate the need for `shell_exec sed/head/tail` hacks:

```json
{"path": "src/react.c", "start_line": 100, "end_line": 200}
```

- **1-based indexing** — matches editor line numbers
- **Negative start_line** — `start_line: -20` reads last 20 lines (tail behavior)
- **Line numbers in output** — each line prefixed with its number (`100: static void ...`)
- **total_lines in response** — helps model decide whether to use ranges on next call
- **Backward compatible** — no parameters = full file read

Empirical data from nash sessions showed **66% of all shell_exec calls** (1,643 out of 2,484) were `sed -n`/`head`/`tail` file reading hacks. This feature eliminates them.

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
mode = "edrm"                             # off | on | edrm
budget = -1                               # -1=unrestricted, 0=none, N>0=max tokens

[embedding]
type = "onnx"                             # onnx | ollama | openai | none
model_path = "~/models/all-MiniLM-L6-v2"  # ONNX model directory

[limits]
shell_timeout = 30                        # seconds
shell_max_output = 1048576                # bytes
file_max_size = 52428800                  # 50MB

[memory]
recall_min_score = 0.05                   # normalized [0, 1] threshold
memory_index_max = 50                     # max entries in memory index
max_skills_per_query = 3                  # skills loaded per query
max_lessons_per_query = 2                 # lessons loaded per query
max_strategies_per_query = 2              # strategies loaded per query
max_antipatterns_per_query = 1            # anti-patterns loaded per query
memory_synthesis = true                   # enable query-time synthesis
prune_min_score = 0.35                    # Bayesian pruning threshold
prune_min_evidence = 3                    # min recalls before pruning
consolidation_threshold = 0.82            # cosine threshold for dedup

[context]
context_eviction_pct = 70                 # evict when context > 70% full

[search]
engine = "duckduckgo"                     # duckduckgo | searxng
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
- **ONNX Runtime** (optional) — local embedding inference

### Build

```bash
cd nash
make
```

### Run

```bash
# Interactive TUI mode
./nash

# Single query mode
./nash -p "fix the memory leak in tools.c"

# With custom API endpoint
./nash --api http://localhost:8080
```

---

## Session Structure

```
~/.nash/
├── config.toml              # Configuration
├── memory/                  # Persistent memory (git-backed)
│   ├── lesson:*.json        # Lessons learned
│   ├── strategy:*.json      # Reusable procedures
│   ├── skill:*.json         # Domain knowledge
│   ├── anti-pattern:*.json  # What not to do
│   └── .git/                # Full history
├── store/                   # Content-addressed artifacts (SHA-256)
│   ├── a1b2c3d4...          # Tool outputs, errors, etc.
│   └── ...
└── sessions/
    └── 1779970830.40871/    # Session directory
        ├── journal.jsonl    # Full event log
        ├── scratchpad.md    # Persistent working notes
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
| [Generative Agents](https://arxiv.org/abs/2304.03442) | 2023 | Composite scoring (recency × importance × relevance) | Hybrid scoring with semantic + substring + importance |
| [Memory Survey](https://arxiv.org/abs/2404.13501) | 2024 | Five critical memory operations including validation | Bayesian validation scoring (hits/misses) |
| [CALMem](https://arxiv.org/abs/2605.20724) | 2026 | Token-budget-adaptive injection (MOIM) | Memory refresh every 3 steps with context-aware re-evaluation |
| [Mem-π](https://arxiv.org/abs/2605.21463) | 2026 | Generative memory policy, learned abstention +59% | Query-time synthesis with semantic abstention ("NONE") |
| [DeferMem](https://arxiv.org/abs/2605.22411) | 2026 | Query-time evidence distillation | Memory synthesis produces faithful, self-contained guidance |
| [MemForest](https://arxiv.org/abs/2605.23986) | 2026 | Temporal indexing, memory relevance changes over time | Adaptive memory refresh during react loop |
| [MemFail](https://arxiv.org/abs/2605.26667) | 2026 | Weak memory injection hurts performance | Bayesian scoring filters low-quality memories |
| [MemMorph](https://arxiv.org/abs/2605.26154) | 2026 | Raw storage insufficient, needs active management | Post-loop pruning + consolidation |

### Cognitive Architecture
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [EDRM / Entropy Phase Transitions](https://arxiv.org/abs/2605.22873) | 2026 | Entropy dynamics predict reasoning need | EDRM routing for thinking mode (probe → route → generate) |
| [TriMem](https://arxiv.org/abs/2605.19952) | 2026 | Three-tier memory (working/episodic/semantic) | Scratchpad (working) + journal (episodic) + memory (semantic) |
| ["Language Models Need Sleep"](https://arxiv.org/abs/2605.26099) | 2026 | Dreaming/consolidation essential for memory health | Post-loop Bayesian pruning + dedup + consolidation |

### Skill Extraction
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [CODESKILL](https://arxiv.org/abs/2605.25430) | 2026 | RL-trained skill extraction from completions | Post-task reflection extracts reusable lessons/strategies |
| [MUSE-Autoskill](https://arxiv.org/abs/2605.27366) | 2026 | Self-evolving skill library | Skills recalled semantically per query, refined via validation |

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

Nash is a personal project focused on exploring what's possible with local LLMs as autonomous coding agents. The codebase is intentionally compact (~34K lines of C) and self-contained.

Key design principles:
- **No Python dependencies** — single compiled binary
- **Local-first** — works with llama.cpp, no cloud required
- **Research-grounded** — every major design decision cites its research basis
- **Self-improving** — the agent learns from every task via persistent memory
- **Full audit trail** — every action is stored, referenced, and inspectable
