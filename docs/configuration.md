# Configuration & Sessions

## Configuration

Nash uses TOML configuration at `~/.nash/config.toml`. API keys are stored separately in `~/.nash/credentials.toml` (chmod 0600). Run `nash --setup` for an interactive wizard that configures providers, embedding, and credentials on first use:

```toml
# Switch providers by changing one line
[routing]
default = "my-local"                      # name of provider to use
#default = "vertex-opus"                  # uncomment to switch
#worker = "local-worker"                  # separate provider for subtasks

# Named providers (define once, reference by name)
[providers.my-local]
type = "local"
api_base = "http://192.168.1.18:8080"    # llama.cpp server
# api_base = "http://localhost:11434"     # Ollama

[providers.vertex-opus]
type = "vertex"
model_id = "claude-opus-4-6"
project_id = "my-gcp-project"            # Vertex AI project
region = "global"                         # Vertex AI region
caching = true                            # prompt caching

[providers.openai-gpt4]
type = "openai"
model_id = "gpt-4o"
# api_key_env = "OPENAI_API_KEY"          # or set in credentials.toml
# context_size = 200000                   # context window (0 = auto)
# chars_per_token = 3.5                   # chars per token ratio

[thinking]
mode = "yes"                              # yes | no | edrm (adaptive)
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
# isolated = false                        # fully isolated -- no global memory access

[session]
# max_indexed_sessions = 0               # max sessions in memory index (0 = no limit)

[search]
engine = "searxng"                        # SearXNG (auto-started via podman/docker)
# searxng_url = "http://localhost:8080"   # custom SearXNG instance
```

---

## Content-Addressed Store & Journal

Every tool output, error, and metadata entry is stored in a **content-addressed store** (`~/.nash/store/`) using SHA-256 hashing. Session directories contain symlinks (`R0S0`, `R0S1`, ...) pointing to store entries.

The **journal** (`journal.jsonl`) records every event with:
- React loop number and step
- Tool name and parameters
- Store reference (ref alias)
- Size, line count
- Failed flag and error message
- Tool call ID (for API threading)

All journal entries have store refs -- no more `+ ?: memory_context` entries. Every step is viewable and inspectable in the TUI.

---

## Checkpoint/Resume

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

---

## Session Management

### Named Sessions

Use `/name PROJECT` to create a human-readable symlink to the current session:
```
~/.nash/sessions/my-refactor -> ~/.nash/sessions/1779970830.40871
```
Named sessions can be resumed with `--session ~/.nash/sessions/my-refactor`.

### Lazy Sessions (Headless Mode)

In headless mode (`-p QUERY`), session directories are created lazily -- only when the first journal entry is written. Empty sessions are automatically cleaned up on exit (`rmdir` if empty). This prevents clutter from quick queries that produce no artifacts.

### Session Auto-Detection

In TUI mode, if the current working directory contains a `journal.jsonl`, nash resumes that session automatically. This is intentionally disabled in headless mode to prevent a child `nash -p` process (spawned via `shell_exec`) from hijacking its parent's session.

---

## Session Structure

```
~/.nash/
|-- config.toml              # Configuration
|-- credentials.toml         # API keys (chmod 0600, created by --setup)
|-- memory/                  # Persistent memory -- GLOBAL layer (git-backed)
|   |-- lesson:*.json        # Lessons learned
|   |-- strategy:*.json      # Reusable procedures
|   |-- skill:*.json         # Domain knowledge
|   |-- fact:*.json          # Concrete data
|   |-- task:*.json          # Ongoing work
|   |-- anti-pattern:*.json  # What not to do
|   |-- .last_dream          # Timestamp of last dream consolidation
|   +-- .git/                # Full history
|-- workspaces/              # Per-workspace memory isolation
|   |-- work-acme/
|   |   +-- memory/          # WORKSPACE layer (work-only, git-backed)
|   +-- personal/
|       +-- memory/          # WORKSPACE layer (personal-only)
|-- models/                  # Per-model profiles
|   +-- *.toml               # e.g., qwen3.toml, claude.toml
|-- playbooks/               # Custom playbooks (dream.yaml auto-seeded)
|   |-- dream.yaml
|   |-- reflect.yaml
|   |-- digest.yaml
|   |-- health.yaml
|   |-- prune.yaml
|   |-- retrospect.yaml
|   +-- self-harness.yaml
|-- regression/              # Regression test query banks
|   +-- *.yaml               # held-in / held-out query banks
|-- runs/                    # Playbook run logs (JSONL)
|   +-- *.jsonl              # e.g., 1779970830.dream.jsonl
|-- searxng/                 # SearXNG container config (auto-created)
|   +-- settings.yml         # Persistent search engine settings
|-- store/                   # Content-addressed artifacts (SHA-256)
|   |-- a1b2c3d4...          # Tool outputs, errors, etc.
|   +-- ...
|-- postmortem.md            # Latest failure analysis report
+-- sessions/
    |-- my-project -> 1779970830.40871  # Named session symlink
    +-- 1779970830.40871/    # Session directory
        |-- journal.jsonl    # Full event log
        |-- scratchpad.jsonl # Persistent working notes (JSONL, append-only)
        |-- scratchpad.md    # Legacy format (loaded as fallback)
        |-- summary.txt      # Session manifest (journal_manifest() output)
        |-- summary.emb      # Embedding vector for semantic search
        |-- checkpoint.json  # Resume state (if interrupted)
        |-- R0S0 -> ../../store/...  # Step aliases (symlinks)
        |-- R0S1 -> ../../store/...
        +-- ...
```
