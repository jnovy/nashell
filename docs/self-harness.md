# Self-Harness -- Automated Improvement

## Post-Task Reflection

After every completed task, nash runs a **reflection phase** -- a mini react loop that extracts reusable lessons, strategies, and skills:

```
System: "Perform CAUSAL ANALYSIS (not narrative summary)..."
1. What assumptions held or almost failed?
2. What hidden variables or context mattered most?
3. What observations were initially ignored?
4. What search branches were pruned -- correctly or incorrectly?
5. What representation or mental model was key to success?
6. What reusable invariant or principle generalizes beyond this task?
```

The model calls `memory_store` to persist lessons, then `done` to finish reflection. Failed tasks get a different prompt focused on failure analysis.

After reflection and scratchpad pruning, nash generates a searchable session summary by calling `journal_manifest()` and embedding the output as `summary.txt` + `summary.emb`. This makes the session discoverable via `memory_search` and `/? query` for all future sessions.

## Self-Harness -- Automated Weakness Mining & Validation

Inspired by [Self-Harness, arXiv:2606.09498], nash includes a full self-improvement loop:

### Postmortem Analysis (Weakness Mining)

Scans session journals to identify recurring failure patterns:

```bash
nash --postmortem                    # analyze last 50 sessions
nash --postmortem-sessions 100       # analyze last 100 sessions
```

Failure signatures are clustered by `(terminal_cause, mechanism, tool)`:
- **Terminal causes**: tool_error, step_limit, null_result, cycling, empty_result
- **Mechanisms**: file_edit_mismatch, unread_ref, shell_retry, context_eviction, wrong_tool, hallucination, spec_violation

Produces evidence bundles for LLM-driven proposal generation.

### Step-Level Trajectory Scoring (SWE-Shepherd)

Inspired by SWE-Shepherd [arXiv:2604.10493], the postmortem now includes **step-level productivity scoring** -- each tool call in a session is classified as:

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

### Regression Testing (Validation Gate)

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
D_in >= 0 AND D_ho >= 0 AND max(D_in, D_ho) > 0
```

```bash
nash --regression                              # run all tests
nash --regression --split held-in              # held-in only
nash --validate-harness baseline               # save baseline
nash --validate-harness compare                # compare against baseline
```

### Tunable Surfaces

Self-harness tunable parameters exposed in config:
- `recall_blend_semantic` / `recall_blend_substring` -- memory scoring blend weights
- `vscore_exponent` -- Bayesian validation power-law exponent
- `tool_retry_limit` -- max consecutive errors before forced strategy switch
- `cycling_window` / `cycling_threshold` -- cycling detection sensitivity

### EvolveMem -- Retrieval Quality Telemetry

Inspired by EvolveMem [arXiv:2605.13941], nash now logs **memory retrieval quality telemetry** after each task. A `memory_quality` journal entry records:

- Which memories were recalled (keys)
- Task outcome (success/failure)
- Cold-start count (memories with zero evidence, vscore=0.5)
- Cold-start percentage

The self-harness playbook includes a 4th pass ("Retrieval Quality Diagnosis") that scans these telemetry entries to diagnose retrieval configuration issues:
- High cold-start rate -> decrease `vscore_exponent`
- Low success rate with recalled memories -> increase `recall_min_score`
- Too many injections -> decrease `max_*_per_query`

This closes the feedback loop between memory retrieval outcomes and retrieval configuration, enabling data-driven tuning of the retrieval parameters.

## Prompt Optimization -- `--optimize`

After creating a model profile, run `--optimize` to automatically tune the `system_prompt_extra` field for your specific model. This implements the Self-Harness iterative loop ([arXiv:2606.09498](https://arxiv.org/abs/2606.09498)):

1. **Weakness Mining** -- cluster failures from regression runs by signature
2. **Harness Proposal** -- generate K diverse, minimal candidate edits to `system_prompt_extra`
3. **Proposal Validation** -- accept only non-regressive edits (held-in does not degrade, held-out improves)

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
| `--epochs N` | 1 | Multi-epoch training -- repeat the full optimization N times |
| `--edit-budget N` | 4 | Initial edit budget L_0 (cosine decay to floor across rounds) |

The optimizer uses query banks from `~/.nash/regression/` (the same ones used by `--regression`). If no query banks exist, a seed set is auto-generated on first run.

### Standard Workflow for Onboarding a Local Model

Running `--optimize` should be a **standard step** when onboarding any new local model -- not an optional afterthought. Research on harness self-improvement ([RHI, arXiv:2607.15524](https://arxiv.org/abs/2607.15524)) found that a few optimization iterations substantially raise the performance ceiling of low-reasoning-effort agents, often **exceeding the maximum-reasoning-effort setting** while reducing inference cost by up to 60%. The gains come primarily from improved context management rather than longer reasoning traces -- exactly what `system_prompt_extra` controls.

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

The optimizer writes accepted prompt edits directly into the model profile's `system_prompt_extra` field. Each accepted edit is non-regressive by construction -- held-in score never decreases, and at least one split improves.

### When to Re-optimize

Re-run `--optimize` when:
- **Upgrading a model** -- a new Qwen or LLaMA release may have different failure modes
- **Changing the task domain** -- switching from coding to research tasks may need different prompting
- **After adding new tools** -- the model may need guidance on when/how to use them
- **After significant config changes** -- new memory thresholds, eviction policies, or tool restrictions
