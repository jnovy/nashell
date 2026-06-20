Name:           nash
Version:        0.1.0
Release:        1%{?dist}
Summary:        Autonomous coding agent in C — New Agentic Shell

# TODO: Set the correct license once a LICENSE file is added upstream
License:        MIT
URL:            https://github.com/jnovy/nash
# Source0:      https://github.com/jnovy/nash/archive/v%%{version}/nash-%%{version}.tar.gz
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel
BuildRequires:  readline-devel
BuildRequires:  ncurses-devel
BuildRequires:  onnxruntime-devel
BuildRequires:  vim-common
# vim-common provides xxd, needed to embed playbook YAML files at build time

Requires:       libcurl
Requires:       openssl-libs
Requires:       readline
Requires:       ncurses-libs
Requires:       onnxruntime

# Bundled libraries (included in source, not linked from system)
Provides:       bundled(cJSON) = 1.7.19

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
  - 18 built-in tools (file I/O, search, web fetch, memory, image analysis)
  - ncurses TUI with markdown rendering, step expansion, keyboard navigation
  - Multiple LLM providers: local (llama.cpp), OpenAI, Anthropic, Vertex AI
  - Semantic memory with Bayesian pruning and pinning
  - Playbook system for automated multi-pass workflows (dream, health, etc.)
  - Session journaling with checkpoint/resume support
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
    LDFLAGS="%{build_ldflags} -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -lonnxruntime"

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
    LDFLAGS="%{build_ldflags} -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -lonnxruntime"

%files
%doc README.md
%{_bindir}/nash
%{_datadir}/%{name}/

%changelog
* Fri Jun 19 2026 Jindrich Novy <jnovy@redhat.com> - 0.1.0-1
- Initial RPM package
