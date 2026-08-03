# Agents -- Scheduled Autonomous Tasks

Agents are workspace-bound, cron-scheduled wrappers around [playbooks](playbooks.md). While a playbook defines *what* to do (passes, prompts, react overrides), an agent defines *when*, *where*, and *how* to run it -- adding scheduling, workspace binding, sensitivity levels, provider overrides, timeout enforcement, and execution history.

```yaml
name: daily-digest
summary: "Summarize workspace activity from the last 24 hours"
description: |
  Scans recent sessions and memory changes, produces a Markdown
  summary, and stores it as the agent result.

workspace: my-project        # which workspace to bind to (Tier 1/2 only)
schedule: "0 8 * * *"        # cron: 8:00 AM daily
timeout: 300                 # kill after 5 minutes
enabled: true                # set to false to disable without deleting

version: "1.0"
provider: anthropic           # per-agent provider override
sensitivity: internal         # public | internal | confidential
tags: [daily, summary]

react:
  standalone: true
  max_steps: 30

passes:
  - label: "Summarize"
    prompt: |
      Scan the workspace {{workspace_dir}} for recent changes.
      Produce a concise daily digest for workspace {{workspace_name}}.
```

Not every playbook needs to be an agent. Use `/play` or `--play` to run playbooks directly. Wrap a playbook in an agent when you need scheduling, workspace binding, or operational metadata.

## Agent YAML Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | filename | Agent name (derived from filename if omitted) |
| `summary` | string | - | One-line description shown in `/agent list` |
| `description` | string | - | Multi-line help/usage shown in `/agent show` |
| `schedule` | string | `manual` | Cron expression or alias (see below) |
| `timeout` | int | 0 | Max execution time in seconds (0 = no limit) |
| `enabled` | bool | true | Set to false to skip during scans |
| `workspace` | string | `_system` | Workspace binding (Tier 1/2 agents only; Tier 3 infers from directory) |
| `version` | string | - | Version string for tracking |
| `provider` | string | - | Provider override (references `[providers.*]` in config.toml) |
| `sensitivity` | string | `public` | Data classification: `public`, `internal`, `confidential` |
| `tags` | list | - | Organizational tags shown in agent list |

All standard [playbook fields](playbooks.md) (`passes`, `react`, `session_mode`, `scratchpad_mode`, etc.) are also supported -- an agent YAML *is* a playbook YAML with extra operational fields.

## Schedule Expressions

Standard 5-field cron format (`minute hour day-of-month month day-of-week`) plus convenience aliases:

| Expression | Meaning |
|-----------|---------|
| `0 8 * * *` | Daily at 8:00 AM |
| `*/15 * * * *` | Every 15 minutes |
| `0 2 * * 0` | Weekly on Sunday at 2:00 AM |
| `0 0 1 * *` | Monthly on the 1st at midnight |
| `0 9-17 * * 1-5` | Hourly during business hours, weekdays |
| `@startup` | Every time the agent scheduler runs |
| `@hourly` | Alias for `0 * * * *` |
| `@daily` | Alias for `0 2 * * *` |
| `@weekly` | Alias for `0 2 * * 0` |
| `@monthly` | Alias for `0 2 1 * *` |
| `manual` | Never automatically scheduled; run only via `/agent run` |

Cron fields support wildcards (`*`), steps (`*/N`), ranges (`N-M`), and lists (`N,M`).

## Three-Tier Discovery

Agents are discovered from three locations, scanned in order. When multiple agents share the same ID, the highest tier wins (like systemd unit overrides):

| Tier | Path | Priority | Workspace binding |
|------|------|----------|-------------------|
| 1 | `/usr/share/nash/agents/` | Lowest | Explicit `workspace:` field (default: `_system`) |
| 2 | `~/.nash/agents/` | Medium | Explicit `workspace:` field (default: `_system`) |
| 3 | `~/.nash/workspaces/NAME/agent/` | Highest | Inferred from directory path |

**Tier 1** is for vendor/RPM-shipped agents (read-only system directory).

**Tier 2** is for user-global agents that aren't tied to a specific workspace directory structure.

**Tier 3** is for workspace-local agents (the most common case). Place your YAML file in `~/.nash/workspaces/my-project/agent/my-agent.yaml` and the workspace is inferred automatically.

### Overriding and masking

A Tier 3 agent with the same name as a Tier 1 agent overrides it completely. To *disable* a lower-tier agent without replacing it, create a symlink to `/dev/null`:

```bash
# Mask a vendor-shipped agent
ln -s /dev/null ~/.nash/agents/unwanted-agent.yaml
```

### Nested workspaces

Tier 3 supports nested workspace directories (e.g., `~/.nash/workspaces/rh/container-tools/agent/build-check.yaml` produces agent ID `rh/container-tools/build-check`). The scanner recurses up to 8 levels deep.

## Template Variables

Agent playbooks have access to all standard [playbook template variables](playbooks.md) plus agent-specific ones:

| Variable | Description |
|----------|-------------|
| `{{workspace_name}}` | Bound workspace name (e.g., `my-project`) |
| `{{workspace_dir}}` | Full path to workspace directory |
| `{{agent_id}}` | Full agent ID (e.g., `my-project/daily-digest`) |
| `{{arguments}}` | Full argument string from `/agent run ID args...` |
| `{{arg1}}`, `{{arg2}}`, ... | Individual space-delimited argument tokens |

Arguments are validated at launch time -- if a pass template references `{{arg3}}` but only 2 arguments were provided, the agent fails immediately with a clear error message and usage hint.

## Sensitivity Levels

Sensitivity controls where agent data flows:

| Level | Constraint |
|-------|-----------|
| `public` | No restrictions (default) |
| `internal` | Requires a local provider (rejects cloud APIs) |
| `confidential` | Local provider required + bridge delivery suppressed (no Telegram/Matrix notifications) |

An agent with `sensitivity: internal` or higher that is configured to use a cloud provider will be rejected at execution time with a clear error.

## Running Agents

### From the TUI

```
/agent                    # list all agents
/agent list               # same as above
/agent show daily-digest  # detailed view of one agent
/agent run daily-digest   # execute immediately (bypasses schedule)
/agent run 3              # run by index number from the list
/agent run my-agent arg1 arg2   # run with arguments
/agent due                # show which agents are due now
/agent history            # execution history (newest first)
/agent history daily-digest     # history filtered to one agent
/agent result daily-digest      # show latest output
```

### From the CLI

```bash
# Run all due agents (respects schedules)
nash --agent

# Run a specific agent by ID (bypasses schedule)
nash --agent --agent-id my-project/daily-digest

# Run with arguments
nash --agent --agent-id my-project/analyze "arg1 arg2"
```

### Daemon mode

When running `nash --agent` from cron or a systemd timer, all due agents are executed sequentially. The flow is:

1. **Scan** -- three-tier discovery finds all agent YAML files
2. **Load** -- merge persisted state from `~/.nash/agent/queue.json`
3. **Schedule** -- compute next-due times, mark which agents are due
4. **Execute** -- run each due agent via `playbook_worker()` (headless, no TUI)
5. **Save** -- update queue.json with execution results (atomic write)

## Execution Details

- Agents run headless (no TUI) when invoked via `--agent`; from the TUI via `/agent run`, they run in the background with live status updates
- Each agent gets its own workspace with two-layer memory (workspace-specific + global fallback)
- ONNX embeddings are initialized per-agent when configured
- Per-agent provider override creates an isolated provider instance
- Results are symlinked to `~/.nash/agent/results/{agent_id}/latest.md` (no duplication on disk)
- Execution history is appended to `~/.nash/agent/history.jsonl`
- Queue state (last run time, status, next due) is persisted in `~/.nash/agent/queue.json`

## Mailbox Integration

When nash is running with bridge threads (Telegram, Matrix), agent results are automatically routed through the mailbox system for delivery. Agents with `sensitivity: confidential` suppress bridge delivery entirely -- their results are stored locally only.

## Examples

### Parameterized agent with required arguments

```yaml
name: analyze-cve
summary: "Analyze a CVE for RHEL/Fedora impact"
description: |
  Usage: /agent run analyze-cve CVE-2024-12345
  Analyzes the given CVE identifier for security impact on
  Red Hat products. Requires one argument: the CVE ID.

schedule: manual
provider: anthropic
sensitivity: internal

react:
  standalone: true
  max_steps: 40

passes:
  - label: "Research CVE"
    prompt: |
      Research {{arg1}} and analyze its security impact on
      RHEL and Fedora. Check NVD, Red Hat security advisories,
      and upstream patches.
  - label: "Write report"
    prompt: |
      Write a concise security impact report for {{arg1}}
      based on your research.
```

Run with: `/agent run analyze-cve CVE-2024-12345`

### Workspace-local scheduled agent

```yaml
name: build-check
summary: "Verify the project builds cleanly"
schedule: "0 */4 * * *"    # every 4 hours
timeout: 120

react:
  max_steps: 15

passes:
  - label: "Build and test"
    prompt: |
      In workspace {{workspace_dir}}, run `make clean && make`
      and report any build errors. If the build succeeds, run
      `make test` and report results.
```

Place at `~/.nash/workspaces/my-project/agent/build-check.yaml` -- workspace is inferred automatically.

## File Layout

```
~/.nash/
  agents/                          # Tier 2: user-global agents
    my-global-agent.yaml
  agent/                           # Agent runtime state
    queue.json                     # Persisted schedule state
    history.jsonl                  # Execution history log
    results/
      my-project/daily-digest/
        latest.md -> /path/to/session/result.md
  workspaces/
    my-project/
      agent/                       # Tier 3: workspace-local agents
        daily-digest.yaml
        build-check.yaml

/usr/share/nash/agents/            # Tier 1: vendor/RPM-shipped agents
  system-health.yaml
```
