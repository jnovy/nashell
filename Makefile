VERSION ?= 0.1.2

# Platform-specific linker and loader conventions.  Keep CC overridable: Apple
# Clang is sufficient, and requiring a versioned Homebrew GCC only makes the
# build needlessly fragile.
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
  PLATFORM_DEFINES := -D_DEFAULT_SOURCE
  SHARED_EXT := so
  SHARED_FLAG := -shared
  SONAME_FLAG := -Wl,-soname,libnash.so.0
  RPATH_ORIGIN := $$ORIGIN
  EXPORT_DYNAMIC := -rdynamic
  DL_LIB := -ldl
  NCURSES_LIB := -lncursesw
  PLUGIN_EXT := so
  BIN_RPATH := -Wl,-rpath,'$(RPATH_ORIGIN)'
else ifeq ($(UNAME_S),Darwin)
  PLATFORM_DEFINES := -D_DARWIN_C_SOURCE
  SHARED_EXT := dylib
  SHARED_FLAG := -dynamiclib
  SONAME_FLAG := -Wl,-install_name,@rpath/libnash.0.dylib
  RPATH_ORIGIN := @loader_path
  EXPORT_DYNAMIC :=
  DL_LIB :=
  NCURSES_LIB := -lncurses
  PLUGIN_EXT := dylib
  BIN_RPATH := -Wl,-rpath,'$(RPATH_ORIGIN)' -Wl,-rpath,'@loader_path/../lib'
  BREW_PACKAGES := ncurses readline openssl@3 utf8proc onnxruntime
  BREW_CFLAGS := $(foreach p,$(BREW_PACKAGES),$(shell brew --prefix $(p) 2>/dev/null | sed 's|^|-I|; s|$$|/include|'))
  BREW_LDFLAGS := $(foreach p,$(BREW_PACKAGES),$(shell brew --prefix $(p) 2>/dev/null | sed 's|^|-L|; s|$$|/lib|'))
else
  $(error Unsupported platform: $(UNAME_S))
endif

CC      ?= gcc
CFLAGS  ?= -Wall -g -Wextra -Wunused-function -O2 -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L $(PLATFORM_DEFINES)
CFLAGS  += $(BREW_CFLAGS)
# ONNX Runtime: requires onnxruntime-devel (headers) to build.
# For linking, use pip-installed libonnxruntime if no system package.
ORT_LIB := $(shell python3 -c "import onnxruntime; import os; print(os.path.dirname(onnxruntime.__file__) + '/capi')" 2>/dev/null)
ifneq ($(ORT_LIB),)
  ORT_LDFLAGS = -L$(ORT_LIB) -Wl,-rpath,$(ORT_LIB) -lonnxruntime
else
  ORT_LDFLAGS = -lonnxruntime
endif

# Device subsystem (VNC, HEVC streaming, Tesseract OCR) is now a separate
# plugin: nash-tool-device-control.  See ~/agents/nash-tool-device-control/
LDFLAGS ?= $(BREW_LDFLAGS) $(EXPORT_DYNAMIC) -lcurl -lcrypto -lreadline $(NCURSES_LIB) -lpthread -lm -lutf8proc $(DL_LIB) $(ORT_LDFLAGS)

# AddressSanitizer for heap corruption detection (opt-in: make SANITIZE=1)
ifdef SANITIZE
CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
endif

# Default data directory (playbooks, etc.) -- /usr/share/nash for installed builds
NASH_DATADIR ?= /usr/share/nash
CFLAGS  += -DNASH_DATADIR='"$(NASH_DATADIR)"'

SRC     = src/main.c src/str.c src/cJSON.c \
          src/journal.c src/store.c src/llm.c src/tools.c src/react.c \
          src/react_context.c \
          src/react_checkpoint.c src/react_reflection.c \
          src/react_error.c src/react_eviction.c \
          src/react_cycling.c \
          src/react_view.c \
          src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_headless.c \
          src/ui_state.c src/ui_md_gen.c src/ui_nav.c src/ui_event.c \
          src/tui.c src/md_render.c \
          src/md_diff.c src/md_osc8.c \
          src/memory.c src/mem_git.c \
          src/workspace.c \
          src/embedding.c \
          src/embedding_onnx.c \
          src/nash_log.c \
          src/yaml_parse.c \
          src/playbook.c \
          src/regression.c \
          src/postmortem.c \
          src/prompt_optimize.c \
          src/scratchpad.c \
          src/session_index.c \
          src/tool_file.c \
          src/tool_search.c \
          src/tool_notes.c \
          src/tool_memory.c \
          src/tool_web.c \
          src/tool_image.c \
          src/tool_todo.c \
          src/todo_core.c \
          src/session_search.c \
          src/mailbox.c \
          src/telegram.c \
          src/md_html.c \
          src/matrix.c \
          src/compress.c \
          src/html_extract.c \
          src/searxng.c \
          src/banner.c \
          src/commands.c \
          src/cmd_todo.c \
          src/cmd_agents.c \
          src/cmd_tool.c \
          src/agents.c \
          src/repomap.c \
          src/tool_subtask.c \
          src/completion.c \
          src/subprocess.c \
          src/tool_plugin.c \
          src/setup.c \
          src/predict.c \
          src/harness_metrics.c \
          src/fswatch_linux.c \
          src/fswatch_kqueue.c \
          src/fswatch_noop.c \
          src/mw_builtin.c
OBJ     = $(SRC:.c=.o)
BIN     = nash

ifeq ($(UNAME_S),Linux)
  LIB_REAL = libnash.so.$(VERSION)
  LIB_SONAME = libnash.so.0
  LIB_LINKER = libnash.so
else
  LIB_REAL = libnash.$(VERSION).dylib
  LIB_SONAME = libnash.0.dylib
  LIB_LINKER = libnash.dylib
endif

all: $(LIB_REAL) $(BIN)

# Header dependencies -- ALL .o files depend on ALL headers.
# This is conservative but safe: changing any header recompiles everything.
HDRS    = $(wildcard src/*.h)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Library objects (everything except main.c for linking with tests)
LIB_SRC = $(filter-out src/main.c, $(SRC))
LIB_OBJ = $(LIB_SRC:.c=.o)

# Shared library: everything except main.c
$(LIB_REAL): $(LIB_OBJ)
	$(CC) $(SHARED_FLAG) $(SONAME_FLAG) -o $@ $^ $(LDFLAGS)
	ln -sf $(LIB_REAL) $(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(LIB_LINKER)

# Binary: main.o links against libnash.so
$(BIN): src/main.o $(LIB_REAL)
	$(CC) $(CFLAGS) -o $@ $< -L. -lnash $(BIN_RPATH) $(LDFLAGS)

# Test binaries
TEST_BIN = tests/test_memory tests/test_store tests/test_config \
           tests/test_str tests/test_journal tests/test_memory_context \
           tests/test_spec tests/test_compaction \
           tests/test_lifecycle tests/test_memory_query \
           tests/test_compress tests/test_semantic_scoring \
           tests/test_breadcrumbs tests/test_optimizer \
           tests/test_reflection tests/test_tool_plugin \
           tests/test_tool_plugin_dlopen \
           tests/test_cycling tests/test_rollback \
           tests/test_predict tests/test_harness_metrics \
           tests/test_workspace tests/test_subtask_context \
           tests/test_plan_tracking \
           tests/test_onnx_embed \
           tests/test_fswatch \
           tests/test_tool_failure

# Sample plugin shared objects for dlopen testing
SAMPLE_PLUGINS = tests/sample_plugin.$(PLUGIN_EXT) tests/sample_plugin_bad_abi.$(PLUGIN_EXT) \
                 tests/sample_plugin_multi.$(PLUGIN_EXT)

tests/sample_%.$(PLUGIN_EXT): tests/sample_%.c src/tool_plugin.h src/cJSON.h $(LIB_REAL)
	$(CC) $(SHARED_FLAG) -fPIC $(CFLAGS) -I src -o $@ $< -L. -lnash

# dlopen test depends on sample .so files
tests/test_tool_plugin_dlopen: tests/test_tool_plugin_dlopen.c $(LIB_REAL) $(SAMPLE_PLUGINS)
	$(CC) $(CFLAGS) -I src -o $@ $< -L. -lnash -Wl,-rpath,'$(RPATH_ORIGIN)/..' $(LDFLAGS)

tests/test_%: tests/test_%.c $(LIB_REAL)
	$(CC) $(CFLAGS) -I src -o $@ $< -L. -lnash -Wl,-rpath,'$(RPATH_ORIGIN)/..' $(LDFLAGS)

test: $(TEST_BIN)
	@echo "=== Running tests ==="
	@failures=0; \
	for t in $(TEST_BIN); do \
		echo "--- $$t ---"; \
		if ./$$t; then echo "PASS"; else echo "FAIL"; failures=$$((failures+1)); fi; \
	done; \
	echo "=== $$failures failures ==="; \
	exit $$failures

# Verify the Linux build from macOS without leaving container-built objects in
# the working tree.  The named container is reused after its first setup.
TEST_CONTAINER ?= nash-test-model
NASH_MODEL_DIR ?= $(HOME)/.nash/models/all-MiniLM-L6-v2
test-container:
	@if podman container exists $(TEST_CONTAINER) 2>/dev/null && \
	  ! podman inspect -f '{{range .Mounts}}{{if eq .Destination "/root/.nash/models/all-MiniLM-L6-v2"}}{{.Source}}{{end}}{{end}}' $(TEST_CONTAINER) | grep -Fxq '$(NASH_MODEL_DIR)'; then \
	  podman rm -f $(TEST_CONTAINER); \
	fi; \
	if podman container exists $(TEST_CONTAINER) 2>/dev/null; then \
	  podman start $(TEST_CONTAINER) 2>/dev/null || true; \
	else \
	  podman run --name $(TEST_CONTAINER) -d \
	    -v $(CURDIR):/workspace:Z -w /workspace \
	    -v $(NASH_MODEL_DIR):/root/.nash/models/all-MiniLM-L6-v2:ro,Z \
	    registry.fedoraproject.org/fedora:latest sleep infinity; \
	  podman exec $(TEST_CONTAINER) dnf install -y gcc make libcurl-devel openssl-devel readline-devel ncurses-devel utf8proc-devel onnxruntime-devel; \
	fi
	@status=0; \
	podman exec $(TEST_CONTAINER) bash -c "make clean && make && make test" || status=$$?; \
	$(MAKE) clean; \
	exit $$status

clean:
	rm -f $(OBJ) $(BIN) $(LIB_REAL) $(LIB_SONAME) $(LIB_LINKER) $(TEST_BIN) $(SAMPLE_PLUGINS)
	rm -f libnash.so* libnash*.dylib
	rm -rf tests/plugin_dir tests/*.dSYM

# Source tarball for RPM builds (matches spec Source0: nash-VERSION.tar.zst)
dist:
	git archive --format=tar --prefix=nash-$(VERSION)/ HEAD | zstd -o nash-$(VERSION).tar.zst

# Format all C source files
fmt:
	git ls-files -z '*.c' '*.h' | xargs -0 clang-format -i

# Install nash binary, library, and data files
DESTDIR ?=
PREFIX  ?= /usr/local
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 755 $(LIB_REAL) $(DESTDIR)$(PREFIX)/lib/
	ln -sf $(LIB_REAL) $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME)
	ln -sf $(LIB_REAL) $(DESTDIR)$(PREFIX)/lib/$(LIB_LINKER)
	install -d $(DESTDIR)$(NASH_DATADIR)/generic-skills
	install -m 644 data/generic-skills/*.json $(DESTDIR)$(NASH_DATADIR)/generic-skills/
	install -d $(DESTDIR)$(NASH_DATADIR)/playbooks
	install -m 644 playbooks/*.yaml $(DESTDIR)$(NASH_DATADIR)/playbooks/

.PHONY: all clean test test-container dist fmt install
