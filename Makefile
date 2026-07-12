CC      ?= gcc
CFLAGS  ?= -Wall -g -Wextra -Wunused-function -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
# ONNX Runtime: use pip-installed libonnxruntime if no system package
ORT_LIB := $(shell python3 -c "import onnxruntime; import os; print(os.path.dirname(onnxruntime.__file__) + '/capi')" 2>/dev/null)
ifneq ($(ORT_LIB),)
  ORT_LDFLAGS = -L$(ORT_LIB) -Wl,-rpath,$(ORT_LIB) -lonnxruntime
else
  ORT_LDFLAGS = -lonnxruntime
endif

# HEVC streaming: detect libx265 + libde265 for continuous capture.
# Set HAVE_X265=0 to force-disable even if libraries are present.
HAVE_X265 ?= $(shell pkg-config --exists x265 libde265 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_X265),1)
  X265_CFLAGS  = -DHAVE_X265 $(shell pkg-config --cflags x265 libde265 2>/dev/null)
  X265_LDFLAGS = $(shell pkg-config --libs x265 libde265 2>/dev/null)
else
  X265_CFLAGS  =
  X265_LDFLAGS =
endif
CFLAGS += $(X265_CFLAGS)

# Tesseract OCR: native perception pipeline (replaces perception.py).
# Set HAVE_TESSERACT=0 to force-disable even if libraries are present.
HAVE_TESSERACT ?= $(shell pkg-config --exists tesseract lept 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_TESSERACT),1)
  TESS_CFLAGS  = -DHAVE_TESSERACT $(shell pkg-config --cflags tesseract lept 2>/dev/null)
  TESS_LDFLAGS = $(shell pkg-config --libs tesseract lept 2>/dev/null)
else
  TESS_CFLAGS  =
  TESS_LDFLAGS =
endif
CFLAGS += $(TESS_CFLAGS)

# VNC backend uses direct RFB protocol (no external VNC library).
# Requires: libjpeg (JPEG encoding), zlib (Tight encoding decompression),
#           OpenSSL/libcrypto (VNC DES authentication — already linked).
LDFLAGS ?= -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm -ljpeg -lz -lutf8proc $(ORT_LDFLAGS) $(X265_LDFLAGS) $(TESS_LDFLAGS)

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
          src/tools_registry.c \
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
          src/agents.c \
          src/repomap.c \
          src/display.c \
          src/display_vnc.c \
          src/input.c \
          src/input_vnc.c \
          src/device.c \
          src/stream.c \
          src/tool_device.c \
          src/tool_subtask.c \
          src/perception.c
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
          src/tools_registry.c \
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
          src/agents.c \
          src/repomap.c \
          src/display.c \
          src/display_vnc.c \
          src/input.c \
          src/input_vnc.c \
          src/device.c \
          src/stream.c \
          src/tool_device.c \
          src/tool_subtask.c \
          src/perception.c
LIB_OBJ = $(LIB_SRC:.c=.o)

# Test binaries
TEST_BIN = tests/test_memory tests/test_store tests/test_config \
           tests/test_str tests/test_journal tests/test_memory_context \
           tests/test_spec tests/test_compaction \
           tests/test_lifecycle tests/test_memory_query \
           tests/test_compress tests/test_semantic_scoring \
           tests/test_breadcrumbs tests/test_optimizer

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

# Standalone perception test binary (OCR analysis on a single image)
perception: src/perception.c src/cJSON.c
	$(CC) $(CFLAGS) -D__PERCEPTION_TEST -o $@ $^ $(TESS_LDFLAGS) -lm

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN) perception src/dream_yaml.inc

.PHONY: all clean test perception
