# Building & Usage

## Dependencies

- **C11 compiler** (gcc or clang)
- **libcurl** -- HTTP client for LLM API calls
- **OpenSSL** (libcrypto) -- SHA-256 for content-addressed store
- **readline** -- command-line input
- **ncursesw** -- TUI rendering (wide-char support)
- **pthreads** -- concurrent inference and TUI threads
- **ONNX Runtime** (optional) -- local embedding inference
- **podman or docker** (optional) -- SearXNG container auto-start for `web_search`

## Build

```bash
cd nash
make                 # builds nash binary + libnash.so (soname-versioned)
make dist            # creates source tarball (tar.zst)
make test            # runs unit tests
sudo make install    # installs to /usr/local (binary, library, headers, playbooks)
```

The shared library uses standard soname versioning: `libnash.so` -> `libnash.so.0` -> `libnash.so.<version>`. A `nash-devel` RPM subpackage is available for plugin development, providing headers and the linker symlink.

## Run

```bash
# Interactive TUI mode
./nash

# Single query mode (headless)
./nash -p "fix the memory leak in tools.c"

# With custom API endpoint
./nash --api http://localhost:8080

# Run a playbook (e.g., memory consolidation)
./nash --play dream

# Resume an existing session
./nash --session ~/.nash/sessions/1779970830.40871

# Custom data directory
./nash --data-dir /path/to/nash-data

# Dump fully-resolved spec (all config layers applied)
./nash --spec

# Load a spec overlay and run
./nash --load-spec my-tuned-spec.toml -p "fix the bug"

# Named workspace (memories isolated from other workspaces)
./nash -w work-acme                   # or --workspace work-acme

# Fully isolated workspace (no global memory recall)
./nash -w personal --isolated

# List all available workspaces
./nash -wl                             # or --workspace-list

# Interactive setup wizard (first-time configuration)
./nash --setup
```

## Self-Harness Commands

```bash
# Run regression test suite
./nash --regression
./nash --regression --split held-in

# Validate harness changes
./nash --validate-harness baseline
./nash --validate-harness compare

# Postmortem failure analysis
./nash --postmortem
./nash --postmortem-sessions 100
```

## CLI Reference

Usage: `nash [OPTIONS] [PATH]`

`PATH` is an optional positional argument specifying a working directory.
When given, nash changes into that directory and enables repo map injection
for codebase-aware context. Without `PATH`, no repo map is generated.

### General

| Flag | Argument | Description |
|------|----------|-------------|
| `--help` | | Show usage summary and exit |
| `--setup` | | Run interactive first-time setup wizard |
| `--data-dir` | `PATH` | Data directory (default: `~/.nash/`) |
| `--session` | `DIR` | Resume an existing session directory |
| `--spec` | | Dump fully-resolved config (all layers applied) and exit |
| `--load-spec` | `FILE` | Load a spec TOML file as a config overlay before running |

### Provider Selection

| Flag | Argument | Description |
|------|----------|-------------|
| `--api` | `URL` | LLM server URL; creates an ad-hoc local provider and selects it |
| `--provider` | `NAME` | Use a named provider defined in `[providers.*]` in config |

### Query and Playbook

| Flag | Argument | Description |
|------|----------|-------------|
| `-p`, `--query` | `QUERY` | Run a single query in headless mode and exit |
| `--play` | `NAME` | Run a named playbook and exit (e.g. `--play dream`) |
| `--validate-playbook` | `NAME` | Validate a playbook definition and exit |

### Workspace

| Flag | Argument | Description |
|------|----------|-------------|
| `-w`, `--workspace` | `NAME` | Activate a named workspace (memory segregation) |
| `-wl`, `--workspace-list` | | List all available workspaces and exit |
| `--isolated` | | Fully isolate workspace -- no global memory recall |

### Agents

| Flag | Argument | Description |
|------|----------|-------------|
| `--agent` | | List all discovered agents and their status |
| `--agent` | `ID [ARGS]` | Run a specific agent by ID, with optional arguments |
| `--agent --due` | | Scan all workspaces, run agents that are due, then exit |
| `--agent --dry-run` | | Show what agents would run without executing them |

`--dry-run` and `--due` are modifiers used together with `--agent`.

### Messaging and Daemon

| Flag | Argument | Description |
|------|----------|-------------|
| `--mailbox` | | Enable file-based mailbox for `user_ask` tool in `-p` mode |
| `--daemon` | | Watch mailbox inbox for task files (implies `--mailbox`) |
| `--telegram` | | Telegram Bot bridge (implies `--daemon`) |
| `--matrix` | | Matrix bridge (implies `--daemon`) |
| `--mailbox-timeout` | `N` | Timeout in seconds for `user_ask` answers (0 = wait forever) |

`--telegram` and `--matrix` each imply `--daemon`, which implies `--mailbox`.

### Self-Harness

| Flag | Argument | Description |
|------|----------|-------------|
| `--regression` | | Run the regression test suite |
| `--split` | `SPLIT` | Filter regression set: `held-in` or `held-out` |
| `--validate-harness` | `MODE` | Validate harness changes: `baseline` or `compare` |
| `--postmortem` | | Analyze recent session failures and print a report |
| `--postmortem-sessions` | `N` | Number of sessions to scan for postmortem (default: 50) |

### Prompt Optimization

| Flag | Argument | Description |
|------|----------|-------------|
| `--optimize` | `BUDGET` | GEPA prompt optimization (`light`, `medium`, `heavy`, or a number) |
| `--reflect-model` | `MODEL` | Reflection LM to use during optimization |
| `--epochs` | `N` | SkillOpt multi-epoch training runs (default: 1) |
| `--edit-budget` | `N` | Initial edit budget L_0 for cosine decay (default: 4) |

## Testing

```bash
make test    # runs unit tests: test_memory, test_store, test_config, test_str,
             #   test_journal, test_memory_context, test_spec, test_compaction,
             #   test_compress, test_lifecycle, test_memory_query, test_optimizer,
             #   test_semantic_scoring, test_breadcrumbs, test_reflection,
             #   test_tool_plugin, test_tool_plugin_dlopen

# Integration tests (requires a running LLM server)
./tests/run_integration.sh --api http://localhost:8080
```

---

## Contributing

See the [Contributing section in README](../README.md#contributing) for design principles and contribution guidelines.
