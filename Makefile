CC      ?= gcc
CFLAGS  ?= -Wall -g -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
# ONNX Runtime: use pip-installed libonnxruntime if no system package
ORT_LIB := $(shell python3 -c "import onnxruntime; import os; print(os.path.dirname(onnxruntime.__file__) + '/capi')" 2>/dev/null)
ifneq ($(ORT_LIB),)
  ORT_LDFLAGS = -L$(ORT_LIB) -Wl,-rpath,$(ORT_LIB) -lonnxruntime
else
  ORT_LDFLAGS = -lonnxruntime
endif

LDFLAGS ?= -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm $(ORT_LDFLAGS)

SRC     = src/main.c src/str.c src/cJSON.c \
          src/journal.c src/store.c src/llm.c src/tools.c src/react.c \
          src/react_context.c \
          src/react_checkpoint.c src/react_reflection.c \
          src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_tui.c \
          src/ui_state.c src/tui.c src/md_render.c \
          src/memory.c \
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
          src/tools_registry.c \
          src/tool_file.c \
          src/tool_search.c \
          src/tool_notes.c \
          src/tool_memory.c \
          src/tool_web.c \
          src/mailbox.c \
          src/compress.c \
          src/html_extract.c \
          src/searxng.c
OBJ     = $(SRC:.c=.o)
BIN     = nash

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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
          src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_tui.c src/ui_state.c src/tui.c src/md_render.c src/memory.c \
          src/workspace.c \
          src/embedding.c src/embedding_onnx.c \
          src/nash_log.c \
          src/yaml_parse.c src/playbook.c \
          src/regression.c src/postmortem.c \
          src/prompt_optimize.c \
          src/scratchpad.c \
          src/tools_registry.c \
          src/tool_file.c \
          src/tool_search.c \
          src/tool_notes.c \
          src/tool_memory.c \
          src/tool_web.c \
          src/mailbox.c \
          src/compress.c \
          src/html_extract.c \
          src/searxng.c
LIB_OBJ = $(LIB_SRC:.c=.o)

# Test binaries
TEST_BIN = tests/test_memory tests/test_store tests/test_config \
           tests/test_str tests/test_journal tests/test_memory_context \
           tests/test_spec

tests/test_%: tests/test_%.c $(LIB_OBJ)
	$(CC) $(CFLAGS) -I src -o $@ $< $(LIB_OBJ) $(LDFLAGS)

test: $(TEST_BIN)
	@echo "=== Running tests ==="
	@failures=0; \
	for t in $(TEST_BIN); do \
		echo "--- $$t ---"; \
		if ./$$t; then echo "PASS"; else echo "FAIL"; failures=$$((failures+1)); fi; \
	done; \
	echo "=== $$failures failures ==="

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN) src/dream_yaml.inc

.PHONY: all clean test
