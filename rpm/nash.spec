Name:           nash
Version:        0.1.0
Release:        4%{?dist}
Summary:        Autonomous coding agent in C - New Agentic Shell

# TODO: Set the correct license once a LICENSE file is added upstream
License:        MIT
URL:            https://github.com/jnovy/nash
# Source0:      https://github.com/jnovy/nash/archive/v%%{version}/nash-%%{version}.tar.gz
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel
BuildRequires:  readline-devel
BuildRequires:  ncurses-devel
BuildRequires:  libjpeg-turbo-devel
BuildRequires:  zlib-ng-compat-devel
BuildRequires:  utf8proc-devel
BuildRequires:  onnxruntime-devel
# HEVC streaming (continuous capture)
BuildRequires:  x265-devel
BuildRequires:  libde265-devel
# Tesseract OCR (native perception pipeline)
BuildRequires:  tesseract-devel
BuildRequires:  leptonica-devel
BuildRequires:  vim-common
# vim-common provides xxd, needed to embed playbook YAML files at build time

Requires:       libcurl
Requires:       openssl-libs
Requires:       readline
Requires:       ncurses-libs
Requires:       libjpeg-turbo
Requires:       zlib-ng-compat
Requires:       utf8proc
Requires:       onnxruntime
Requires:       x265-libs
Requires:       libde265
Requires:       tesseract-libs
Requires:       leptonica

# Bundled libraries (included in source, not linked from system)
Provides:       bundled(cJSON) = 1.7.19
Provides:       bundled(tomlc99)

%description
Nash is a fully autonomous coding agent implemented in C. It connects to any
OpenAI-compatible LLM server (llama.cpp, OpenAI, Anthropic, Vertex AI) and
executes multi-step coding tasks through a ReAct (Reason + Act) loop with
persistent memory, a TUI interface, and research-grounded cognitive
architecture.

Unlike wrapper-based agents, nash is a single compiled binary with zero Python
dependencies. It runs locally with local models, maintains long-term memory
across sessions, and learns from every task it completes.

Key features:
  - 21 built-in tools (file I/O, search, web fetch, memory, image analysis, device control, subtask)
  - ncurses TUI with markdown rendering, step expansion, in-page search
  - Multiple LLM providers: local (llama.cpp), OpenAI, Anthropic, Vertex AI
  - Semantic memory with Bayesian pruning, ONNX embeddings, and BM25 scoring
  - Aider-style repo map for structural codebase context
  - Playbook system for automated multi-pass workflows (dream, health, etc.)
  - Session journaling with checkpoint/resume and episodic search
  - Context compaction with configurable eviction policies
  - Content-addressed store with full audit trail

%prep
%autosetup -n %{name}-%{version}

%build
# Clean any pre-compiled objects (source tarball may contain stale .o files)
make clean || true

# Generate embedded playbook data (requires xxd from vim-common)
make src/dream_yaml.inc

%make_build CC=gcc \
    CFLAGS="%{optflags} -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE" \
    LDFLAGS="%{build_ldflags} -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -ljpeg -lz -lutf8proc -lonnxruntime -lx265 -lde265 -ltesseract -lleptonica"

%install
install -D -p -m 0755 nash %{buildroot}%{_bindir}/nash

# Install playbook YAML files
install -d %{buildroot}%{_datadir}/%{name}/playbooks
for pb in playbooks/*.yaml; do
    install -p -m 0644 "$pb" %{buildroot}%{_datadir}/%{name}/playbooks/
done

# Install SearXNG config
install -d %{buildroot}%{_datadir}/%{name}/config/searxng
install -p -m 0644 config/searxng/* %{buildroot}%{_datadir}/%{name}/config/searxng/

# Install helper scripts
install -d %{buildroot}%{_datadir}/%{name}/scripts
install -p -m 0755 scripts/nash-mailbox-bridge.sh %{buildroot}%{_datadir}/%{name}/scripts/

# Install documentation
install -d %{buildroot}%{_docdir}/%{name}
install -p -m 0644 README.md %{buildroot}%{_docdir}/%{name}/

%check
# Build and run the test suite
make test CC=gcc \
    CFLAGS="%{optflags} -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE" \
    LDFLAGS="%{build_ldflags} -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -ljpeg -lz -lutf8proc -lonnxruntime -lx265 -lde265 -ltesseract -lleptonica"

%files
%doc README.md
%{_bindir}/nash
%{_datadir}/%{name}/

%changelog
* Wed Jul 22 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-4
- feat: subtask tool for isolated child react loops
- feat: device_control tool for GUI automation (screenshot, click, type, scroll, drag)
- feat: tab-completion for TUI slash commands
- feat: agent system - three-tier discovery (vendor, user, workspace), standalone mode
- feat: named providers and role-based routing config
- feat: playbook validation, script passes, error policy
- feat: unified subprocess fork/exec/timeout with process group cleanup
- feat: perception - multi-pass strip OCR with contrast normalization
- feat: optimizer - flip gating, blind critic, signal stopping
- feat: repo map opt-in via positional PATH arg
- feat: Ctrl-W (delete word backwards) in TUI query input
- fix: glob_search poll-based timeout to prevent indefinite blocking
- fix: shell_exec default timeout 300->30s with per-call timeout param
- fix: status bar shows actual worker model instead of default
- fix: tab rendering, horizontal scroll, viewport clipping in md_render
- Update tool count from 19 to 21

* Mon Jul 06 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-3
- Add missing dependencies: libjpeg-turbo, zlib-ng-compat, utf8proc, x265,
  libde265, tesseract, leptonica, pkgconfig

* Sun Jun 22 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-2
- feat: /todo command and persistent todo tool
- feat: Aider-style repo map for structural codebase context
- feat: BM25 scoring for memory search relevance
- feat: unified memory_search (replaces memory_recall + session_search)
- feat: session_grep with POSIX Extended Regular Expression support
- feat: /ms TUI command for full-parameter memory search
- feat: in-page text search with ?-prefix, match highlighting, n/N navigation
- feat: configurable provider retry policy via config.toml
- feat: configurable eviction policy struct (replaces 20+ scattered #defines)
- feat: per-chunk journal RAG with ONNX embeddings
- feat: pre-compaction warning (alerts model before eviction fires)
- feat: Qwen XML tool call parsing as fallback
- feat: top_p and top_k sampling parameter support
- feat: UDCS uncertainty decomposition prompt
- feat: event-driven retrieval and auto-promotion
- feat: arrow-down past last history entry restores in-progress text
- fix: context compaction rewrite - mark-sweep eviction, death spiral
  elimination, 30+ bug fixes across 15 review passes
- fix: memory subsystem - 13 design/logic flaws (3 critical, 3 high)
- fix: replace hardcoded buffer sizes with named constants
- fix: UTF-8 safe truncation and LLM error display
- fix: cycle-detection refinement and portability improvements
- Update tool count from 18 to 19

* Fri Jun 19 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-1
- Initial RPM package
