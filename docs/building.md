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

## Testing

```bash
make test    # runs unit tests: test_memory, test_store, test_config, test_str, test_journal, test_memory_context, test_spec

# Integration tests (requires a running LLM server)
./tests/run_integration.sh --api http://localhost:8080
```

---

## Contributing

Nash is a personal project focused on exploring what's possible with local LLMs as autonomous coding agents. The codebase is intentionally compact and self-contained.

Key design principles:
- **No Python dependencies** -- single compiled binary
- **Local-first** -- works with llama.cpp, no cloud required
- **Research-grounded** -- every major design decision cites its research basis
- **Self-improving** -- the agent learns from every task via persistent memory
- **Full audit trail** -- every action is stored, referenced, and inspectable
