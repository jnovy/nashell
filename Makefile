CC      ?= gcc
CFLAGS  ?= -Wall -g -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -lcurl -lcrypto -lreadline -lncursesw -lpthread -lm

SRC     = src/main.c src/str.c src/cJSON.c \
          src/journal.c src/store.c src/llm.c src/tools.c src/react.c \
          src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_tui.c \
          src/ui_state.c src/tui.c src/md_render.c \
          src/memory.c
OBJ     = $(SRC:.c=.o)
BIN     = nash

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Header dependencies — ALL .o files depend on ALL headers.
# This is conservative but safe: changing any header recompiles everything.
# For a 15-file project this adds <1s to rebuilds.
HDRS    = $(wildcard src/*.h)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Library objects (everything except main.c for linking with tests)
LIB_SRC = src/str.c src/cJSON.c src/journal.c src/store.c \
          src/llm.c src/tools.c src/react.c src/config.c src/toml.c \
          src/provider.c src/provider_local.c \
          src/provider_openai.c src/provider_anthropic.c \
          src/frontend_tui.c src/ui_state.c src/tui.c src/md_render.c src/memory.c
LIB_OBJ = $(LIB_SRC:.c=.o)

# Test binaries
TEST_BIN = tests/test_memory tests/test_store tests/test_config \
           tests/test_str tests/test_journal

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
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

.PHONY: all clean test
