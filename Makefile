CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -lcurl -lcrypto -lreadline

SRC     = src/main.c src/str.c src/arena.c src/cJSON.c \
          src/journal.c src/store.c src/llm.c src/tools.c src/react.c \
          src/config.c src/toml.c \
          src/frontend_tui.c \
          src/memory.c
OBJ     = $(SRC:.c=.o)
BIN     = nash

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
