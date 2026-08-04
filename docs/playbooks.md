# Playbooks -- Multi-Pass Task Orchestration

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
- **Per-pass react overrides** -- max_steps, memory injection, reflection, compaction, scoring
- **Tool filtering** -- whitelist (`tools_allow`) or blacklist (`tools_block`) per pass
- **Template variables** -- `{{memory_dir}}`, `{{model}}`, `{{session_dir}}`, `{{nash_dir}}`, custom vars
- **Session modes** -- `per-pass` (fresh session each pass) or `shared` (one session)
- **Scratchpad modes** -- `shared` (carry across passes) or `isolated` (fresh each pass)
- **Post hooks** -- `prune_memory`, `commit`
- **Inter-pass pause** -- optionally wait for user confirmation between passes
- **Custom system prompts** -- per-pass `system_prompt` with append or replace modes
- **Standalone mode** -- suppress all host-local context for portable, self-contained agents
- **Required tools gate** -- per-pass `required_tools` list verifies that specified tools were actually invoked after a pass completes; catches the "call narrated, not made" failure mode where the model describes tool usage without executing it (inspired by [SIGIL, arXiv:2607.27309](https://arxiv.org/abs/2607.27309))

Bundled playbooks: `dream`, `reflect`, `digest`, `health`, `prune`, `retrospect`, `self-harness`

Run with `--play NAME` or from the TUI.

## Custom System Prompts

Each pass can define a custom system prompt that either appends to or replaces the base system prompt:

```yaml
passes:
  - label: "Analyze CVE"
    system_prompt_mode: append    # "append" (default) or "replace"
    system_prompt: |
      You are a CVE vulnerability analyst specializing in Red Hat products.
      Focus on security impact assessment, not general coding.
    prompt: |
      Analyze {{arg1}} for impact on RHEL and Fedora...
```

- **`append`** (default) -- the custom text is appended to the base system prompt under a `[PLAYBOOK-SPECIFIC RULES]` header. The LLM still receives all standard tool-usage rules (store-and-reference pattern, file_edit conventions, etc.), plus your additions.
- **`replace`** -- the base system prompt is replaced entirely with your custom text. Model-specific rules from the profile TOML are still appended. Use this when you want full control over the system prompt, but note that the LLM will not receive nash's standard tool-usage instructions unless you include them yourself.

## Standalone Mode

By default, nash injects up to 12 layers of contextual preprompting before the user query: memory index, pinned memories, temporal calendar, episodic recall, skills, lessons, strategies, anti-patterns, associated memories, repo map, scratchpad, and previous result. This is what makes nash agents smarter over time -- but it also ties behavior to the host machine's memory state.

The `standalone` flag suppresses all host-local context, making the YAML fully portable across nash installations:

```yaml
name: analyze-cve
description: "Portable CVE analysis agent"
provider: anthropic

react:
  standalone: true          # suppress all host-local context

passes:
  - label: "Analyze"
    system_prompt_mode: replace
    system_prompt: |
      You are a CVE vulnerability analyst. You have access to
      shell_exec, web_fetch, file_read, file_write, and done.
    prompt: |
      Analyze {{arg1}} for security impact...
```

`standalone: true` is a convenience flag equivalent to:

```yaml
react:
  inject_memory: false       # no pinned memories, skills, lessons, temporal calendar, episodic recall
  inject_prev_result: false  # no result.md from prior runs
  inject_repomap: false      # no codebase structure injection
```

Context layers and what controls them:

| # | Context Layer | Gate Flag | Suppressed by standalone? |
|---|---------------|-----------|--------------------------|
| 1 | System prompt | `system_prompt_mode: replace` | Only if explicitly replaced |
| 2 | Memory index + pinned | `inject_memory` | Yes |
| 3 | Temporal calendar | `inject_memory` | Yes |
| 4 | Episodic recall | `inject_memory` | Yes |
| 5 | Skills / Lessons / Strategies / Anti-patterns | `inject_memory` | Yes |
| 6 | Associated memories (graph walk) | `inject_memory` | Yes |
| 7 | Repo map | `inject_repomap` | Yes |
| 8 | Scratchpad | always on | N/A (empty on fresh run) |
| 9 | Previous result | `inject_prev_result` | Yes |
| 10 | TUI view context | TUI only | N/A (agents are headless) |

Three levels of portability:

1. **`standalone: true`** (recommended) -- suppresses all host-local context. The base system prompt (tool harness rules) is still injected, teaching the LLM how to use nash's tools correctly. Your YAML's prompt is the only variable part.

2. **`standalone: true` + `system_prompt_mode: replace`** (maximum control) -- also replaces the base system prompt. The LLM sees only what is in your YAML. Use this for fully self-contained agent definitions.

3. **`standalone: true` + selective overrides** (hybrid) -- override individual flags after standalone sets them. For example, `standalone: true` with `inject_memory: true` suppresses repo map and previous result but still uses host-local memories.

Individual flags can also be used without `standalone` for fine-grained control:

```yaml
react:
  inject_memory: true        # keep memories (default)
  inject_prev_result: false  # suppress previous result injection
  inject_repomap: false      # suppress repo map injection
```

## Headless-Aware Base Prompt

When running in headless mode (agents, `--play`, `-p`), the base system prompt automatically adapts:

- **Identity** -- "You are an autonomous agent" instead of "autonomous coding agent"
- **Clarification seeking** -- the `user_ask` guidance section is suppressed (there is no interactive user)
- **Result routing** -- adjusted to reflect that results go to `result.md` / mailbox, not a user's screen

This happens automatically -- no YAML configuration needed. It prevents the LLM from attempting to call `user_ask` in headless contexts where no user is present to respond.

## Playbook Run Logs

Every playbook execution is logged to `~/.nash/runs/` as a JSONL file with events:
- `start` -- playbook name, number of passes
- `pass` -- pass index, label, session ID
- `done` -- pass completion status
- `end` -- overall result (ok/fail)

View run history with `/runs` (list) and `/runs show ID` (details) in the TUI.

---

## See Also

- [Configuration & Sessions](configuration.md) - config.toml reference
- [Agents](agents.md) - scheduled execution of playbooks
- [Self-Harness](self-harness.md) - regression testing and prompt optimization
