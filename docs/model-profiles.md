# Model Profiles & Unified Spec

## Model Profiles -- Per-Model Spec Overrides

Model profiles in `~/.nash/models/*.toml` provide **full configuration overrides** per model. When nash detects which model it's talking to (via `config_match_model()` longest-substring match), the matched profile overlays any `config_t` field using sentinel-based inheritance: unset fields (`-1`, `0.0`, `NULL`) inherit from `config.toml`, set fields override it.

This implements the [OpenJarvis](https://arxiv.org/abs/2605.17172) insight that **the model profile IS the local/cloud adapter** -- no "local mode" toggle needed. Switching from Claude to Qwen automatically adjusts temperature, step limits, tool restrictions, memory thresholds, and tool descriptions.

### Layer Resolution Order

```
compile-time defaults -> config.toml -> model profile -> playbook pass -> CLI flags
```

Each layer only overrides fields it explicitly sets. Everything else cascades from the layer below.

### Example: Local Model (Qwen)

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
max_react_steps = 30                   # cap steps -- local models cycle more
cycling_detection = true
tool_retry_limit = 2                   # fail fast
max_reflection_steps = 3

[memory]
recall_min_score = 0.20                # stricter -- inject fewer, more relevant
memory_index_max = 40
max_skills_per_query = 2
max_lessons_per_query = 2

[tools]
block = ["web_search"]                 # block tools the model can't use well

[tools.file_edit]
description = """Edit a file by replacing exact text. CRITICAL: You MUST call
file_read first and copy old_text character-by-character from the output."""

system_prompt_extra = """
[MODEL-SPECIFIC RULES -- Qwen]
- Issue exactly ONE tool call per response.
- After a tool error, do NOT retry the same approach. Switch strategy immediately.
- When calling file_edit, copy old_text from file_read output verbatim.
"""
```

### Example: Cloud Model (Claude)

```toml
match = "claude"
chars_per_token = 3.5
native_context = 200000

[thinking]
mode = "yes"
budget = -1                            # unlimited -- cloud can afford it

# Cloud models are capable -- light touch, inherit config.toml defaults
system_prompt_extra = """
[MODEL-SPECIFIC RULES -- Claude]
- When using file_edit, always file_read first to verify the exact text.
"""
```

### Overridable Fields

| Section | Fields | Sentinel |
|---------|--------|----------|
| `[client]` | `temperature`, `max_tokens` | `0.0` / `0` = inherit |
| `[thinking]` | `mode`, `budget` | `THINKING_UNSET` = inherit |
| `[react]` | `inject_memory`, `inject_prev_result`, `enable_reflection`, `enable_pruning`, `enable_compaction`, `enable_scoring` | `-1` = inherit |
| `[react]` | `max_react_steps`, `max_reflection_steps`, `tool_retry_limit`, `cycling_detection` | `0` / `-1` = inherit |
| `[memory]` | `recall_min_score`, `recall_blend_semantic`, `recall_blend_substring`, `vscore_exponent`, `memory_index_max`, `max_*_per_query`, `context_eviction_pct` | `0.0` / `0` / `-2.0` = inherit |
| `[tools]` | `allow` (whitelist), `block` (blacklist) | `NULL` = inherit (all tools) |
| `[tools.<name>]` | `description` -- per-tool description override | `NULL` = use compiled default |

The special sentinel `-2.0` for `vscore_exponent` exists because both `0.0` (disabled) and `-1.0` are valid values.

### Features
- **Longest-match priority** -- `qwen3-30b` matches before `qwen` for model ID `qwen3-30b-a3b`
- **Playbook cascade** -- profile overrides flow through to all playbook passes as the lowest-priority layer
- **Tool description overrides** -- rewrite tool descriptions for weaker models without recompiling
- **Tool filtering** -- whitelist or blacklist tools per model (small models can't compose complex tools)
- **Native context warnings** -- alerts when server n_ctx is much smaller than model capacity

Example profiles for Claude, Qwen, LLaMA, DeepSeek, Gemma, and Mistral are shown above. Create them at `~/.nash/models/` to customize behavior per model.

---

## Unified Spec -- Reproducible Configuration Snapshots

Inspired by [OpenJarvis](https://arxiv.org/abs/2605.17172) (Stanford, 2026), which formalizes personal AI systems as a composition of five typed primitives (Intelligence, Engine, Agents, Tools & Memory, Learning) bundled into a single versioned "spec," nash implements **full spec serialization and import**.

OpenJarvis showed that when you swap a cloud model for a local one, accuracy drops 25-39 pp because the entire stack was co-designed for the cloud model. Prompt-only tuning recovers just ~5 pp. The solution: jointly optimize across all primitives via a typed spec that captures the *complete* configuration. Nash's layered model profiles implement this -- the profile carries the full "tuned configuration around the model," and `--spec` makes it inspectable.

### `nash --spec` -- Export

Dumps the fully-resolved configuration after all layers are applied (compile defaults -> `config.toml` -> model profile -> CLI flags) as a single TOML document:

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
- **Reproducibility** -- capture exactly what configuration produced a result
- **A/B comparison** -- `nash --spec > before.toml`, make changes, `nash --spec > after.toml`, diff
- **Spec sharing** -- share optimized configs ("here's my coding spec for Qwen3-30B on an M4 Mac")

### `nash --load-spec FILE` -- Import

Loads a TOML spec file as a configuration overlay on top of the existing config. Only specified sections are applied; everything else keeps its current value:

```bash
# Round-trip: export -> modify -> reimport
nash --spec > my-spec.toml
vim my-spec.toml                   # adjust parameters
nash --load-spec my-spec.toml      # apply as overlay

# Verify round-trip fidelity
nash --spec > a.toml
nash --load-spec a.toml --spec > b.toml
diff a.toml b.toml                 # identical
```

Supported overlay sections: `[provider]`, `[client]`, `[thinking]`, `[react]` (flags + limits), `[memory]`, `[tools]` (allow/block/description overrides), `[limits]`, `[memory_belief_entropy]`.

### Mapping to OpenJarvis Primitives

| OpenJarvis Primitive | Nash Spec Section | Implementation |
|---------------------|-------------------|----------------|
| **Intelligence** | `[provider]`, `[thinking]`, model profiles | `provider.c` vtable + `config_match_model()` |
| **Engine** | `[client]`, `[limits]`, `[embedding]` | `config.c` + `embedding.c` |
| **Agents** | `[react]` (flags + limits) | `react.c` + `react_flags_t` |
| **Tools & Memory** | `[tools]`, `[memory]`, `[memory_belief_entropy]` | `tools.c` + `memory.c` |
| **Learning** | Playbooks, regression, postmortem | `playbook.c` + `regression.c` + `postmortem.c` |
