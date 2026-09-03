# ReAct Loop & Tools

Nash implements a full ReAct (Reason + Act) loop that drives autonomous task completion:

```
User Query -> [Plan] -> Tool Call -> Observe Result -> [Reflect] -> Next Tool Call -> ... -> Done
```

- **Native OpenAI tool_calls API** -- uses structured `tool_calls` with `tool_call_id` threading, not JSON-in-content hacks
- **20 built-in tools** -- shell_exec, file_read, file_write, file_edit, grep_search, glob_search, web_fetch, web_search, notes, plan, done, memory_store, memory_search, memory_pin, memory_unpin, memory_delete (default: off), image_analyze, user_ask, subtask, todo, rollback
- **Plugin-based tool registry** (`tool_plugin.h`) -- tools self-register via `__attribute__((constructor))`, formatted per-provider (local/OpenAI/Anthropic), ABI v3 with lifecycle hooks (`init`/`cleanup`)
- **External plugin override** -- external `.so` plugins loaded via `dlopen` can override built-in tools in-place; `tool_plugin_load_dir()` loads all plugins from a directory
- **Dispatch table** -- tool execution via plugin registry lookup, not strcmp chains
- **Tool filtering** -- per-playbook-pass whitelist/blacklist restricts available tools
- **Cycling detection** -- sliding-window detection of repeated tool call patterns (periods 1-4), injects corrective guidance with cycle description, refuses after repeated failures
- **Concatenated tool name recovery** -- when the model emits garbled names (e.g., `shell_execshell_exec`), automatically extracts the longest matching prefix and dispatches correctly
- **Unknown tool recovery** -- when the model generates a non-existent tool name, injects a corrective message listing available tools and lets the model retry
- **Edit transaction rollback** -- tracks all file_edit/file_write operations and can revert to pre-session state
- **Cut-off summarizer** -- when a response is truncated at max_tokens, nash summarizes the partial output and retries with higher limits to prevent lost reasoning
- **Fresh-perspective escape** -- when the agent is stuck in a loop, nash spawns a subtask asking for a fresh perspective on the problem and injects new ideas into the main loop
- **Brainstorm gate** -- before plan creation, the react loop requires a brainstorm phase via `notes(section='brainstorm')` analyzing task difficulty, candidate approaches, and pitfalls; plans without brainstorming are rejected
- **Per-role temperature** -- subtasks can specify a `temperature` override (0.0-1.0) via the subtask tool parameter
- **Dynamic max_tokens escalation** -- default max_tokens is 32768; when thinking exhaustion (truncated extended thinking) is detected, max_tokens automatically doubles up to the provider maximum
- **Pseudocode-before-code norm** -- for algorithm/math tasks, the agent writes pseudocode in `notes(section='pseudocode')` before implementing real code, catching logic errors cheaply

---

## Cycling Detection

Nash detects repetitive tool call patterns using a sliding-window algorithm inspired by [arXiv 2608.00101](https://arxiv.org/abs/2608.00101). The detector maintains a circular buffer of the last 12 action signatures (tool name + parameter hash) and checks for repeating patterns of period 1 through 4:

| Period | Pattern | Example |
|--------|---------|---------|
| 1 | A -> A -> A | `file_read` -> `file_read` -> `file_read` (same file) |
| 2 | A -> B -> A -> B | `file_read` -> `shell_exec` -> `file_read` -> `shell_exec` |
| 3 | A -> B -> C -> A -> B -> C | Three-step repeating loop |
| 4 | A -> B -> C -> D -> A -> B -> C -> D | Four-step repeating loop |

When a cycle is detected, the escalation message includes the cycle description (e.g., "period-2 cycle: file_read -> shell_exec -> file_read -> shell_exec") so the agent understands the specific pattern it needs to break.

Config: `cycling_window` / `cycling_threshold` in `[limits]` section.

### Diagnostic: `tools/scan_cycles`

The `tools/scan_cycles` standalone utility scans historical session journals to analyze cycling patterns across sessions. Useful for diagnosing whether cycling is a systemic issue or model-specific.

---

## Edit Transaction Rollback

The `rollback` tool reverts all `file_edit` and `file_write` changes made during the current session back to their original state. Every file modification records a save-point (pre-edit content stored in the content-addressed store), and rollback restores files to their state before the **first** edit in the session - not intermediate states.

Parameters:
- `reason` (string) - why rolling back (logged to the journal)

When a build command (`make`, `gcc`, `cargo`, etc.) fails via `shell_exec` and there are pending edits in the session, the tool result includes a hint reminding the agent that `rollback` is available.

Use cases:
- A sequence of edits led to build failures and the agent wants to start over cleanly
- An approach turned out to be wrong and all changes need reverting
- Exploratory edits that should not persist

---

## Cut-off Summarizer (Truncation Recovery)

When an LLM response is truncated because it hit the max_tokens limit, the partial output contains reasoning and tool calls that would be lost. Nash detects the `finish_reason: length` signal and automatically:

1. Summarizes the truncated response using a lightweight LLM call to preserve key reasoning
2. Injects the summary back into the conversation as context
3. Retries the request with doubled max_tokens (see Dynamic max_tokens escalation below)

This prevents the common failure mode where an agent loses its chain of thought mid-reasoning and starts over from scratch.

---

## Fresh-Perspective Escape

When cycling detection fires repeatedly and corrective guidance fails to break the loop, nash escalates to a fresh-perspective escape:

1. The react loop detects that the agent has received cycling warnings but continues the same pattern
2. Nash spawns a `subtask` with `context="critic"` asking a fresh model instance to analyze the problem from scratch
3. The subtask receives the original goal and a summary of failed approaches
4. Its response (new ideas, alternative strategies) is injected into the main loop as a user message
5. The main loop continues with the benefit of an outside perspective

This is more effective than corrective guidance alone because the subtask has no prior context bias - it sees the problem with fresh eyes.

---

## Brainstorm Gate

Before allowing plan creation, the react loop enforces a brainstorm-first policy:

1. When the agent calls `plan(op="add_item")` without a prior `notes(op="write", section="brainstorm")` in the session, the plan call is rejected with a message requiring brainstorming first
2. The brainstorm note must analyze:
   - Core difficulty of the task
   - Candidate approaches (at least two)
   - Pitfalls and edge cases to watch for
3. Only after the brainstorm section exists can the agent create a plan

This prevents the agent from jumping straight into implementation without considering the problem space, which is the leading cause of wasted tool calls and dead-end approaches.

---

## Per-Role Temperature

The `subtask` tool accepts an optional `temperature` parameter (0.0-1.0) that overrides the default temperature for that subtask's LLM calls:

- Lower temperature (0.0-0.3) for precise tasks: code implementation, exact search
- Higher temperature (0.5-1.0) for creative tasks: brainstorming, generating alternatives
- Default: inherits from parent session
- Ignored for reasoning models (e.g., o3) that do not support temperature

This allows the agent to tune creativity vs precision per-subtask without affecting the main session.

---

## Dynamic max_tokens Escalation

Nash starts with a default `max_tokens` of 32768 tokens per request. When thinking exhaustion is detected - the model's extended thinking block is truncated mid-stream - nash automatically escalates:

1. First truncation: max_tokens doubles to 65536
2. Second truncation: doubles again to 131072
3. Continues doubling up to the provider's documented maximum
4. If the provider max is reached and output is still truncated, nash logs a warning and proceeds with the partial result

Thinking exhaustion is distinct from output truncation: it specifically means the model ran out of space in its internal reasoning (chain-of-thought) block, not just the visible output. This is detected via provider-specific signals (e.g., Anthropic's `stop_reason: max_tokens` with active thinking blocks).

Config: `max_tokens` in provider section sets the initial value.

---

## Pseudocode-Before-Code Norm

For tasks involving algorithm implementation, mathematical logic, or multi-step data transformations, the react loop encourages writing pseudocode before real code:

1. The agent writes pseudocode in `notes(section='pseudocode')` describing the algorithm
2. This catches logic errors cheaply before committing to a specific implementation
3. The pseudocode serves as documentation that survives context compaction
4. Only after the pseudocode is written does the agent proceed to implement in real code

This norm is enforced via system prompt instructions rather than hard rejection (unlike the brainstorm gate), since not all coding tasks benefit from pseudocode.

---

## Error Recovery

### HTTP 500 -- 3-Tier Retry Strategy

When the LLM server returns HTTP 500 (malformed tool_calls JSON, server crash):

| Tier | Attempt | Strategy | Rationale |
|------|---------|----------|-----------|
| 1 | 2nd | Remove last assistant+tool_result pair | Model's last output was malformed |
| 2 | 3rd | Reformulate scratchpad via LLM | Code blocks in scratchpad confuse JSON generation |
| 3 | 4th | Strip scratchpad entirely | Nuclear option -- remove all context pollution |
| -- | -- | Give up | All recovery strategies exhausted |

Each tier logs a `server_error` entry to the journal with full diagnostics:
- `server_message` -- actual error from the server
- `request_ref` -- raw request body stored in store/ (for post-mortem)
- `response_ref` -- raw server response stored in store/

Retry count and backoff delay are configurable via `provider_max_retries` and `provider_retry_base` in `[limits]` (defaults: 10 retries, 10s base delay).

### Unknown Tool Recovery

When the model generates a non-existent tool name (e.g., `shell_execshell_exec`):
1. `tool_execute()` tries longest-prefix match against the dispatch table
2. If a prefix matches, dispatches to that tool automatically
3. Otherwise returns error with available tools list
4. React loop injects corrective user message and lets the model retry

### Done Result Fallback

When the model puts the summary in `thought` instead of `result` (common with local models):
```c
if ((!result || !result[0]) && thought && thought[0]) {
    result = thought;  // thought IS the answer for done calls
}
```

---

## file_read with Line Ranges

Nash's `file_read` tool supports `start_line` and `end_line` parameters to eliminate the need for `shell_exec sed/head/tail` hacks:

```json
{"path": "src/react.c", "start_line": 100, "end_line": 200}
```

- **1-based indexing** -- matches editor line numbers
- **Negative start_line** -- `start_line: -20` reads last 20 lines (tail behavior)
- **Line numbers in output** -- each line prefixed with its number (`100: static void ...`)
- **total_lines in response** -- helps model decide whether to use ranges on next call
- **Backward compatible** -- no parameters = full file read

---

## See Also

- [Custom Tool Plugins](plugins.md) - external .so plugin API
- [Context Management](context-management.md) - eviction and compression
- [TUI](tui.md) - slash commands and tool management
