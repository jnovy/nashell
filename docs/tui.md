# TUI - Terminal User Interface

Nash provides a full ncurses-based TUI with:

- **Markdown rendering** -- headers, bold, italic, inline code, code blocks, tables, horizontal rules, lists
- **Inline formatting in links** -- tool names rendered in bold, descriptions as inline code
- **Code block clipping** -- long code block lines are clipped to terminal width instead of wrapping across multiple visual rows (prevents overwriting progress indicators and footers)
- **Differential rendering** -- only changed cells are sent to the terminal via ncurses, eliminating visible flicker from the 50ms render loop (full redraws still occur on terminal resize and after OSC8 hyperlink flush)
- **Step expansion** -- click/Enter on a step to expand its full content from the store
- **Streaming output** -- real-time token display during LLM generation
- **Status bar** -- model name, context usage percentage (`ctx 42%`), background jobs count, dream reminder
- **Journal view** -- full session history with react loop headers, step markers (+/x), thoughts
- **Keyboard navigation** -- arrow keys, Page Up/Down, Home/End, Enter to expand/collapse
- **Input history** -- arrow keys browse query history; arrow-down past the last entry restores in-progress text (standard shell/readline behavior)
- **Pause/Resume** -- press Space during inference to pause after the current step; Space or new query to resume
- **Auto-redirect** -- typing a new query during active inference automatically pauses the current task, stashes the new query, and dispatches it immediately when the loop yields -- no "Space then type" dance required
- **Cross-session search** -- type `/?query` to search memory and session history with hybrid scoring (ranked results displayed in the TUI)

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
| `/agent` | List all agents with schedule, status, and due state |
| `/agent show ID` | Detailed view of a single agent (metadata, last run, next due) |
| `/agent run ID [args]` | Execute an agent immediately, bypassing its schedule |
| `/agent due` | Show which agents are due for execution now |
| `/agent history [ID]` | Execution history (newest first); optionally filtered by agent |
| `/agent result ID` | Display the latest output from an agent run |
| `/memory_search QUERY` | Search memory using hybrid scoring; display ranked results in the TUI |
| `/ms QUERY` | Full-parameter memory search (alias: `/memory_search`). Supports `-q` query, `-k` key, `-p` pattern, `-r` regex, `-n` max results, `-d` days. Searches both curated memory (L4) and session journals (L3) |
| `/todo [args]` | Persistent TODO list management (add, list, done, remove, purge) |
| `/tool [sub]` | Tool management: `list`, `show`, `on`, `off`, `reset`, `save`, `load`, `toggle` |
| `/?query` | Search memory and session history (hybrid scoring, ranked results) |
| `/continue` | Resume from checkpoint with the original query |
| `quit` / `exit` | Exit nash |

Unrecognized slash commands (e.g., `/foo`) are rejected with an error displayed in the status bar ("Unknown command: /foo") instead of being silently sent to the LLM as a chat message.

## Tool Management (`/tool`)

The `/tool` command controls which tools are available to the agent during a
session. Tools can be enabled or disabled at runtime, and configurations can
be saved to named profiles for reuse.

| Subcommand | Description |
|------------|-------------|
| `/tool` or `/tool list` | Show all tools with their current on/off status |
| `/tool show NAME` | Display full details for a tool -- description, parameters, status, version, and group |
| `/tool on NAME` | Enable a previously disabled tool |
| `/tool off NAME` | Disable a tool (protected tools `done` and `user_ask` cannot be disabled) |
| `/tool NAME` | Toggle a tool on or off |
| `/tool reset` | Reset all tools to their default state |
| `/tool save PROFILE` | Save the current set of disabled tools to a named profile |
| `/tool load` | List all saved tool profiles |
| `/tool load PROFILE` | Load a saved profile, replacing the current blocked list |

Tool profiles are stored as plain text files (one disabled tool name per line)
in `~/.nash/tool-profiles/`. Profile names must be alphanumeric (a-z, 0-9,
dashes, underscores).

## TODO List (`/todo`)

The `/todo` command manages a persistent per-workspace TODO list stored as
`todo.md` in the workspace directory. Items use Markdown checkbox format
(`- [ ]` for open, `- [x]` for done).

| Subcommand | Description |
|------------|-------------|
| `/todo` or `/todo list` | Display all items in the current workspace |
| `/todo add <text>` | Add a new open item |
| `/todo done <N>` | Mark item number N as completed |
| `/todo remove <N>` | Remove item N entirely (alias: `/todo rm <N>`) |
| `/todo purge` | Remove all completed items, keeping open ones |
| `/todo -w` | Show aggregated TODO items from all workspaces |

The TODO list is also available as a tool for the agent -- the LLM can read
and modify workspace TODOs during autonomous operation.

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

## File Change Detection (fswatch)

Nash uses Linux inotify to monitor the working directory for external file
modifications - edits made outside of nash (e.g., in another editor or by a
build system). When changes are detected, the modified files are recorded in
the same `modified_files` tracking that `file_edit` and `file_write` use, so
the agent sees external changes exactly like its own edits.

The implementation is a thin platform abstraction (`fswatch.h`) with two
backends:

- **Linux** (`fswatch_linux.c`) - uses inotify for efficient kernel-level
  notification with no polling overhead
- **Other platforms** (`fswatch_noop.c`) - no-op fallback; the feature is
  silently unavailable

Key design points:

- **Recursive directory monitoring** - `fswatch_add()` watches the workspace
  root and all subdirectories, spawning a background pthread for the initial
  directory scan so the TUI is not blocked on large trees
- **Watch count ceiling** - capped at 65536 inotify watches
  (`WATCH_MAX_WATCHES`) to prevent resource exhaustion on large repositories
- **Depth limit** - recursive scanning stops at 8 levels deep
  (`WATCH_MAX_DEPTH`) to keep the initial scan bounded
- **Hidden directories skipped** - dotfiles and directories like `.git` are
  excluded from watching
- **Pollable file descriptor** - `fswatch_fd()` returns an fd usable with
  `poll()`/`select()`, integrated into the TUI main loop for efficient
  wake-up instead of busy-waiting
- **Event types** - four bitmask flags: `FSW_MODIFY`, `FSW_CREATE`,
  `FSW_DELETE`, `FSW_RENAME`

The TUI main loop calls `fswatch_drain()` when the inotify fd becomes
readable, which invokes the callback for each pending event. The callback
filters out directory events (only file content changes are tracked) and
feeds changes into the session's `modified_files` array (up to
`INFORM_MAX_FILES` = 32 entries).
