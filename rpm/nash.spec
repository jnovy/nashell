Name:           nash
Version:        0.1.0
Release:        13%{?dist}
Summary:        Autonomous Agentic Harness - New Agentic Shell

License:        MIT
URL:            https://github.com/jnovy/nash
# Source0:      https://github.com/jnovy/nash/archive/v%%{version}/nash-%%{version}.tar.gz
Source0:        %{name}-%{version}.tar.zst

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel
BuildRequires:  readline-devel
BuildRequires:  ncurses-devel
BuildRequires:  utf8proc-devel
BuildRequires:  onnxruntime-devel
BuildRequires:  bash-completion

Requires:       libcurl
Requires:       openssl-libs
Requires:       readline
Requires:       ncurses-libs
Requires:       utf8proc
Requires:       onnxruntime

# Device control plugin (VNC, OCR, HEVC streaming) is now a separate package
Suggests:       nash-tool-device-control

# Bundled libraries (included in source, not linked from system)
Provides:       bundled(cJSON) = 1.7.19
Provides:       bundled(tomlc99)

%description
Nash is a fully autonomous coding agent harness implemented in C. It connects to any
OpenAI-compatible LLM server (llama.cpp, OpenAI, Anthropic, Vertex AI) and
executes multi-step coding tasks through a ReAct (Reason + Act) loop with
persistent memory, a TUI interface, and research-grounded cognitive
architecture.

Unlike wrapper-based agents, nash is a single compiled binary with minimal
dependencies. It runs locally with local models, maintains long-term memory
across sessions, and learns from every task it completes.

Key features:
  - built-in tools (file I/O, search, web fetch, memory, image analysis, device control, subtask)
  - ncurses TUI with markdown rendering, step expansion, in-page search
  - Multiple LLM providers: local (llama.cpp), OpenAI, Anthropic, Vertex AI
  - Semantic memory with Bayesian pruning, ONNX embeddings, and BM25 scoring
  - Aider-style repo map for structural codebase context
  - Playbook system for automated multi-pass workflows (dream, health, etc.)
  - Session journaling with checkpoint/resume and episodic search
  - Context compaction with configurable eviction policies
  - Content-addressed store with full audit trail
  - Plugin system with libnash.so for independent tool development

%package devel
Summary:        Development files for nash plugin development
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
Headers and shared library for developing nash plugins.
Plugins are shared objects (.so) that self-register via
__attribute__((constructor)) and are loaded by nash at runtime.

Build plugins with:
  gcc -shared -fPIC -I%{_includedir}/nash -o plugin.so plugin.c -lnash

%prep
%autosetup -n %{name}-%{version}

%build
# Clean any pre-compiled objects (source tarball may contain stale .o files)
make clean || true

%make_build CC=gcc \
    CFLAGS="%{optflags} -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DNASH_DATADIR='\"%{_datadir}/%{name}\"'" \
    LDFLAGS="%{build_ldflags} -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -lutf8proc -lonnxruntime"

%install
install -D -p -m 0755 nash %{buildroot}%{_bindir}/nash

# Install shared library (versioned)
install -D -p -m 0755 libnash.so.%{version} %{buildroot}%{_libdir}/libnash.so.%{version}
ln -s libnash.so.%{version} %{buildroot}%{_libdir}/libnash.so.0
ln -s libnash.so.0 %{buildroot}%{_libdir}/libnash.so

# Install development headers
install -d %{buildroot}%{_includedir}/nash
install -p -m 0644 src/tool_plugin.h %{buildroot}%{_includedir}/nash/
install -p -m 0644 src/cJSON.h %{buildroot}%{_includedir}/nash/
install -p -m 0644 src/device_control_config.h %{buildroot}%{_includedir}/nash/

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

# Install bash completion
install -D -p -m 0644 scripts/nash-completion.bash \
    %{buildroot}%(pkg-config --variable=completionsdir bash-completion)/nash

# Install documentation
install -d %{buildroot}%{_docdir}/%{name}
install -p -m 0644 README.md %{buildroot}%{_docdir}/%{name}/

%check
# Build and run the test suite
make test CC=gcc \
    CFLAGS="%{optflags} -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DNASH_DATADIR='\"%{_datadir}/%{name}\"'" \
    LDFLAGS="%{build_ldflags} -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -lutf8proc -lonnxruntime"

%ldconfig_scriptlets

%files
%license LICENSE
%doc README.md
%{_bindir}/nash
%{_libdir}/libnash.so.%{version}
%{_libdir}/libnash.so.0
%{_datadir}/%{name}/
%(pkg-config --variable=completionsdir bash-completion)/nash

%files devel
%{_libdir}/libnash.so
%{_includedir}/nash/

%changelog
* Mon Aug 03 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-13
- Add MIT LICENSE file and remove license TODO from spec

* Sun Aug 02 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-12
- Add proper soname versioning to libnash.so. Build with
  -Wl,-soname,libnash.so.0, ship versioned libnash.so.0.1.0 and soname symlink
  libnash.so.0 in main package. Move unversioned libnash.so linker symlink to
  -devel subpackage. ldconfig_scriptlets now functional.

* Thu Jul 30 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-11
- Add interactive setup wizard (--setup) and credentials.toml support. Validate
  API key availability at startup before creating provider.

* Wed Jul 29 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-10
- Add bash completion script for CLI flags and arguments

* Sun Jul 26 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-9
- Switch source tarball from tar.gz to tar.zst. Remove compiled binaries (.so,
  test executables) from git tracking.

* Sun Jul 26 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-8
- Remove embedded dream.yaml blob; ship playbook as regular file with
  system-path fallback via NASH_DATADIR. Drop xxd build dependency.

* Sun Jul 26 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-7
- fix: BuildRequires vim-common -> xxd (xxd split into its own package since
  Fedora 40)

* Sun Jul 26 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-6
- ABI v3: plugin lifecycle hooks (cleanup/init/user_data) in tool_plugin_t.
  Separated device_control into external nash-tool-device-control plugin.
  Removed device subsystem (VNC, HEVC, Tesseract) from core - now a separate .so
  plugin loaded at runtime.

* Sun Jul 26 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-5
- feat: libnash.so shared library and nash-devel subpackage for independent
  plugin development

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
