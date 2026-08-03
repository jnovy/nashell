# nash - Autonomous Agentic Harness - New Agentic Shell

<p align="center">
  <img src="demo/nash-demo.gif" alt="nash demo" width="800">
</p>

**nash** is a agentic harness implemented as a single compiled binary. It connects to any OpenAI-compatible LLM server (llama.cpp, OpenAI, Anthropic, Vertex AI) and executes multi-step coding tasks through a ReAct (Reason + Act) loop with persistent memory, a TUI interface, and research-grounded cognitive architecture.

Unlike wrapper-based agents, nash has minimal runtime dependencies. It runs locally with local models, maintains long-term memory across sessions, and learns from every task it completes.

---

## Key Features

- **20 built-in tools** - file I/O, search, web fetch, memory, image analysis, subtask spawning
- **ncurses TUI** - markdown rendering, step expansion, streaming output, in-page search
- **4 LLM providers** - local (llama.cpp), OpenAI, Anthropic, Vertex AI
- **Persistent memory** - Bayesian-validated, git-backed, workspace-isolated
- **ONNX embeddings** - local semantic search with no API calls
- **Playbook system** - YAML-defined multi-pass workflows (dream, health, reflect, etc.)
- **Agent scheduler** - cron-scheduled autonomous tasks with workspace binding, sensitivity gating, three-tier discovery
- **Self-improvement** - postmortem analysis, regression testing, prompt optimization
- **Session journaling** - checkpoint/resume, episodic search, full audit trail
- **Context management** - importance-tagged eviction, BM25 compression, lossless breadcrumbs
- **Plugin system** - `libnash.so` for independent tool development
- **Research-grounded** - papers were driving the design (see [Research Foundations](docs/research.md))

---

## Architecture

```
+-----------------------------------------------------------+
|                      TUI (ncurses)                        |
|  Markdown rendering - Step expansion - Keyboard nav       |
+-----------------------------------------------------------+
|                   React Loop (react.c)                    |
|  Plan -> Tool Call -> Observe -> Reflect -> Done          |
+----------+----------+-----------+-------------------------+
| Provider |  Memory  |   Tools   |   Journal + Store       |
| local    | semantic | 20 tools  | content-addressed       |
| openai   | Bayesian | registry  | full audit trail        |
| anthropic| event-   | dispatch  | checkpoint/resume       |
| vertex   | driven   | filtering | episodic recall         |
+----------+----------+-----------+-------------------------+
|    Agents - Playbooks - Self-Harness - Model Profiles     |
+-----------------------------------------------------------+
|            LLM Server (llama.cpp / API)                   |
+-----------------------------------------------------------+
```

---

## Quick Start

```bash
# Build
make

# Interactive TUI mode
./nash

# First-time setup wizard
./nash --setup

# Single query (headless)
./nash -p "fix the memory leak in tools.c"

# Run a playbook
./nash --play dream

# Resume a session
./nash --session ~/.nash/sessions/my-project
```

See [Building & Usage](docs/building.md) for full build instructions, dependencies, and CLI reference.

---

## Documentation

| Document | Description |
|----------|-------------|
| [Memory Architecture](docs/memory.md) | Four-tier memory system, Bayesian scoring, embeddings, pruning, dreaming, reactive retrieval, workspaces |
| [ReAct Loop & Tools](docs/react-loop.md) | ReAct loop, 20 built-in tools, plugin registry, error recovery |
| [Multi-Provider Support](docs/providers.md) | Local, OpenAI, Anthropic, Vertex AI provider configuration |
| [Context Management](docs/context-management.md) | Structural reasoning, Harness-1 eviction, scratchpad architecture |
| [TUI](docs/tui.md) | Terminal interface, slash commands, tree branching, SearXNG search |
| [Playbooks](docs/playbooks.md) | YAML multi-pass workflows, standalone mode, custom system prompts |
| [Agents](docs/agents.md) | Cron-scheduled autonomous tasks, three-tier discovery, sensitivity gating, workspace binding |
| [Self-Harness](docs/self-harness.md) | Postmortem analysis, regression testing, prompt optimization |
| [Model Profiles & Spec](docs/model-profiles.md) | Per-model overrides, unified spec export/import |
| [Configuration & Sessions](docs/configuration.md) | config.toml reference, session structure, checkpoint/resume |
| [Building & Usage](docs/building.md) | Dependencies, build, run, CLI reference, testing |
| [Research Foundations](docs/research.md) | 36 papers informing the design |

---

## How It Works

Nash runs a **ReAct loop** - the agent reasons about what to do, executes a tool, observes the result, and repeats until the task is done:

```
User Query -> [Plan] -> Tool Call -> Observe Result -> [Reflect] -> ... -> Done
```

**Memory** persists across sessions in four tiers: context window (volatile), scratchpad (session-persistent), session history (searchable journals), and curated memory (git-backed, Bayesian-validated). The agent learns from every task through post-task reflection and consolidation.

**Context management** uses importance-tagged messages with multi-pass progressive eviction - recoverable content (files, memory) is evicted before irreplaceable observations. Sentence-BM25 compression keeps the most relevant sentences when context pressure hits.

**Self-improvement** closes the loop: postmortem analysis mines failure patterns from session history, regression tests validate changes, and `--optimize` automatically tunes the system prompt for your model.

See the [documentation](#documentation) for deep dives into each subsystem.

---

## Configuration

Nash uses TOML configuration at `~/.nash/config.toml`. Run `nash --setup` for an interactive wizard:

```toml
# Switch providers by changing one line
[routing]
default = "my-local"
#default = "vertex-opus"

# Named providers (define once, reference by name)
[providers.my-local]
type = "local"
api_base = "http://192.168.1.18:8080"    # llama.cpp server

[providers.vertex-opus]
type = "vertex"
model_id = "claude-opus-4-6"
project_id = "my-gcp-project"
region = "global"

[thinking]
mode = "yes"                              # yes | no | edrm (adaptive)

[embedding]
type = "onnx"                             # onnx | ollama | openai | none
model_path = "~/models/all-MiniLM-L6-v2"
```

Per-model profiles in `~/.nash/models/*.toml` override any config field per model. See [Configuration & Sessions](docs/configuration.md) for the full reference and [Model Profiles](docs/model-profiles.md) for profile examples.

---

## Contributing

Nash is a project focused on exploring what's possible with local LLMs as autonomous coding agents. The codebase is intentionally compact and self-contained.

Key design principles:

- **Minimal runtime dependencies** - single compiled binary
- **Local-first** - works with llama.cpp, no cloud required
- **Research-grounded** - every major design decision cites its research basis
- **Self-improving** - the agent learns from every task via persistent memory
- **Full audit trail** - every action is stored, referenced, and inspectable

---

## License

MIT
