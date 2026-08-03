# TUI - Terminal User Interface

Nash provides a full ncurses-based TUI with:

- **Markdown rendering** -- headers, bold, italic, inline code, code blocks, tables, horizontal rules, lists
- **Inline formatting in links** -- tool names rendered in bold, descriptions as inline code
- **Step expansion** -- click/Enter on a step to expand its full content from the store
- **Streaming output** -- real-time token display during LLM generation
- **Status bar** -- model name, context usage percentage (`ctx 42%`), background jobs count, dream reminder
- **Journal view** -- full session history with react loop headers, step markers (+/x), thoughts
- **Keyboard navigation** -- arrow keys, Page Up/Down, Home/End, Enter to expand/collapse
- **Input history** -- arrow keys browse query history; arrow-down past the last entry restores in-progress text (standard shell/readline behavior)
- **Pause/Resume** -- press Space during inference to pause after the current step; Space or new query to resume
- **Auto-redirect** -- typing a new query during active inference automatically pauses the current task, stashes the new query, and dispatches it immediately when the loop yields -- no "Space then type" dance required
- **Cross-session search** -- type `/?query` for incremental scratchpad search, or `/? query` for semantic session history search (embedding-based, searches `summary.emb` across all sessions)

## TUI Slash Commands

| Command | Description |
|---------|-------------|
| `/dream` | Run memory consolidation (alias for `/play dream`) |
| `/play NAME` | Run a named playbook in background (e.g., `/play reflect`) |
| `/play list` | List all available playbooks with descriptions |
| `/name NAME` | Create a named symlink to the current session (`~/.nash/sessions/NAME`) |
| `/cwd DIR` | Change working directory; creates the directory if it doesn't exist (`mkdir -p`) |
| `/runs` | List all playbook run logs (from `~/.nash/runs/`) |
| `/runs show ID` | Display details of a specific playbook run |
| `/memory_search QUERY` | Search memory using hybrid scoring; display ranked results in the TUI |
| `/ms QUERY` | Full-parameter memory search (alias: `/memory_search`). Supports `-q` query, `-k` key, `-p` pattern, `-r` regex, `-n` max results, `-d` days. Searches both curated memory (L4) and session journals (L3) |
| `/workspace NAME` | Switch to a named workspace mid-session; `/workspace` shows current workspace |
| `/?query` | Cross-session scratchpad search (live incremental results) |
| `/? query` | Semantic session history search (embedding-based, shows ranked results) |
| `/continue` | Resume from checkpoint with the original query |
| `quit` / `exit` | Exit nash |

## Tree-Based Branching

Nash supports **non-linear conversation trees**. When the user views a previous react loop's output (`reactRX.md`) and submits a new query, nash branches from that point:

1. The `parent_loop` is set to the viewed react loop
2. A `[Branched from R<N>: "original query"]` context hint is injected
3. The parent loop's result is loaded from the scratchpad
4. Scratchpad sections are filtered non-destructively at injection time -- only sections from the ancestor chain are included

This enables exploring alternative approaches without losing the original conversation path.

## Web Search with SearXNG Auto-Start

The `web_search` tool uses **SearXNG** as its search backend (self-hosted, private).

When SearXNG is configured but not running, nash **automatically starts a SearXNG container** using podman (preferred) or docker:

1. Creates a persistent config directory at `~/.nash/searxng/` with a `settings.yml` enabling JSON API
2. Launches `docker.io/searxng/searxng:latest` with bind-mounted config
3. Waits for the container to respond
4. Tears down the container on nash exit (`web_search_cleanup()`)

The bundled `config/searxng/settings.yml` provides a minimal override that inherits SearXNG defaults while enabling JSON output format and configuring search engines for coding tasks.

Rate limiting protection:

- **Query throttle** -- enforces a minimum 3-second gap between searches to avoid upstream engine rate limiting
- **Nuclear recovery** -- when a search returns 0 results (likely due to upstream bans or CAPTCHAs), automatically restarts the SearXNG container to reset all engine state, then retries the query

## Interactive user_ask Tool

The `user_ask` tool allows the LLM to pause inference and ask the user a clarifying question:

1. The inference thread sets `user_ask_pending` and emits a `REACT_EVENT_USER_ASK` event
2. The TUI displays the question and waits for user input
3. The user's response is passed back to the inference thread via `user_ask_answer`
4. The react loop resumes with the answer injected into context

This enables the agent to resolve ambiguities rather than guessing, particularly useful for tasks with underspecified requirements.
