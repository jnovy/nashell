CC      ?= gcc
CFLAGS  ?= -Wall -g -Wextra -Wunused-function -O2 -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
# ONNX Runtime: use pip-installed libonnxruntime if no system package
ORT_LIB := $(shell python3 -c "import onnxruntime; import os; print(os.path.dirname(onnxruntime.__file__) + '/capi')" 2>/dev/null)
ifneq ($(ORT_LIB),)
  ORT_LDFLAGS = -L$(ORT_LIB) -Wl,-rpath,$(ORT_LIB) -lonnxruntime
else
  ORT_LDFLAGS = -lonnxruntime
endif

# Device subsystem (VNC, HEVC streaming, Tesseract OCR) is now a separate
# plugin: nash-tool-device-control.  See ~/agents/nash-tool-device-control/
LDFLAGS ?= -rdynamic -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -lutf8proc -ldl $(ORT_LDFLAGS)

SRC     = src/main.c src/str.c src/cJSON.c \
          src/journal.c src/store.c src/llm.c src/tools.c src/react.c \
          src/react_context.c \
          src/react_checkpoint.c src/react_reflection.c \
          src/react_error.c src/react_eviction.c \
          src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_tui.c \
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
          src/tool_plugin.c
OBJ     = $(SRC:.c=.o)
BIN     = nash

LIB     = libnash.so

all: $(LIB) $(BIN)

# Embed bundled playbooks as C byte arrays at build time.
# playbooks/dream.yaml → src/dream_yaml.inc (included by playbook.c)
src/dream_yaml.inc: playbooks/dream.yaml
	xxd -i $< > $@

# Header dependencies — ALL .o files depend on ALL headers.
# This is conservative but safe: changing any header recompiles everything.
# For a 15-file project this adds <1s to rebuilds.
HDRS    = $(wildcard src/*.h)

# playbook.o additionally depends on the embedded YAML
src/playbook.o: src/dream_yaml.inc

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Library objects (everything except main.c for linking with tests)
LIB_SRC = src/str.c src/cJSON.c src/journal.c src/store.c \
          src/llm.c src/tools.c src/react.c src/react_context.c \
          src/react_checkpoint.c \
          src/react_reflection.c \
          src/react_error.c src/react_eviction.c \
          src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_tui.c src/ui_state.c src/ui_md_gen.c src/ui_nav.c src/ui_event.c \
          src/tui.c src/md_render.c src/memory.c src/mem_git.c \
          src/md_diff.c src/md_osc8.c \
          src/workspace.c \
          src/embedding.c src/embedding_onnx.c \
          src/nash_log.c \
          src/yaml_parse.c src/playbook.c \
          src/regression.c src/postmortem.c \
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
          src/tool_plugin.c
LIB_OBJ = $(LIB_SRC:.c=.o)

# Shared library: everything except main.c
$(LIB): $(LIB_OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# Binary: main.o links against libnash.so
$(BIN): src/main.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lnash -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

# Test binaries
TEST_BIN = tests/test_memory tests/test_store tests/test_config \
           tests/test_str tests/test_journal tests/test_memory_context \
           tests/test_spec tests/test_compaction \
           tests/test_lifecycle tests/test_memory_query \
           tests/test_compress tests/test_semantic_scoring \
           tests/test_breadcrumbs tests/test_optimizer \
           tests/test_reflection tests/test_tool_plugin \
           tests/test_tool_plugin_dlopen

# Sample plugin shared objects for dlopen testing
SAMPLE_PLUGINS = tests/sample_plugin.so tests/sample_plugin_bad_abi.so \
                 tests/sample_plugin_multi.so

tests/sample_%.so: tests/sample_%.c src/tool_plugin.h src/cJSON.h $(LIB)
	$(CC) -shared -fPIC $(CFLAGS) -I src -o $@ $< -L. -lnash

# dlopen test depends on sample .so files
tests/test_tool_plugin_dlopen: tests/test_tool_plugin_dlopen.c $(LIB) $(SAMPLE_PLUGINS)
	$(CC) $(CFLAGS) -I src -o $@ $< -L. -lnash -Wl,-rpath,'$$ORIGIN/..' $(LDFLAGS)

tests/test_%: tests/test_%.c $(LIB)
	$(CC) $(CFLAGS) -I src -o $@ $< -L. -lnash -Wl,-rpath,'$$ORIGIN/..' $(LDFLAGS)

test: $(TEST_BIN)
	@echo "=== Running tests ==="
	@failures=0; \
	for t in $(TEST_BIN); do \
		echo "--- $$t ---"; \
		if ./$$t; then echo "PASS"; else echo "FAIL"; failures=$$((failures+1)); fi; \
	done; \
	echo "=== $$failures failures ==="

clean:
	rm -f $(OBJ) $(BIN) $(LIB) $(TEST_BIN) $(SAMPLE_PLUGINS) src/dream_yaml.inc
	rm -rf tests/plugin_dir

.PHONY: all clean test
