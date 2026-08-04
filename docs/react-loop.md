# ReAct Loop & Tools

Nash implements a full ReAct (Reason + Act) loop that drives autonomous task completion:

```
User Query -> [Plan] -> Tool Call -> Observe Result -> [Reflect] -> Next Tool Call -> ... -> Done
```

- **Native OpenAI tool_calls API** -- uses structured `tool_calls` with `tool_call_id` threading, not JSON-in-content hacks
- **20 built-in tools** -- shell_exec, file_read, file_write, file_edit, grep_search, glob_search, web_fetch, web_search, notes, plan, done, memory_store, memory_search, memory_pin, memory_unpin, memory_delete (default: off), image_analyze, user_ask, subtask, todo
- **Plugin-based tool registry** (`tool_plugin.h`) -- tools self-register via `__attribute__((constructor))`, formatted per-provider (local/OpenAI/Anthropic), ABI v3 with lifecycle hooks (`init`/`cleanup`)
- **External plugin override** -- external `.so` plugins loaded via `dlopen` can override built-in tools in-place; `tool_plugin_load_dir()` loads all plugins from a directory
- **Dispatch table** -- tool execution via plugin registry lookup, not strcmp chains
- **Tool filtering** -- per-playbook-pass whitelist/blacklist restricts available tools
- **Cycling detection** -- detects repeated identical tool calls, injects corrective guidance, refuses after repeated failures
- **Concatenated tool name recovery** -- when the model emits garbled names (e.g., `shell_execshell_exec`), automatically extracts the longest matching prefix and dispatches correctly
- **Unknown tool recovery** -- when the model generates a non-existent tool name, injects a corrective message listing available tools and lets the model retry

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
