CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -g
CPPFLAGS ?= -Iinclude

SRC := src/main.c src/source.c src/diag.c src/lexer.c src/ast.c src/parser.c src/bash_emit.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := ds

.PHONY: all clean check smoke test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

build/%.o: src/%.c include/ds.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

check: $(BIN)
	./$(BIN) check examples/basic.ds
	! ./$(BIN) check examples/bad.ds >/tmp/ds_bad.out 2>&1

test: $(BIN)
	./tests/run_v0_1.sh

smoke: $(BIN)
	./$(BIN) tokens examples/basic.ds
	./$(BIN) ast examples/basic.ds
	./$(BIN) check examples/basic.ds

clean:
	rm -rf build $(BIN)
