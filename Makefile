CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -g
CPPFLAGS ?= -Iinclude

SRC := src/main.c src/source.c src/diag.c src/lexer.c src/ast.c src/parser.c src/lower.c src/runtime.c src/vm.c src/bash_emit.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := ds

.PHONY: all clean check smoke test test-v0-4 test-v0-5 asan ubsan

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
	./tests/v0_1/run.sh
	./tests/v0_2/run.sh
	./tests/v0_3/run.sh
	./tests/v0_4/run.sh
	./tests/v0_5/run.sh

test-v0-4: $(BIN)
	./tests/v0_4/run.sh

test-v0-5: $(BIN)
	./tests/v0_5/run.sh

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test

ubsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test

smoke: $(BIN)
	./$(BIN) tokens examples/basic.ds
	./$(BIN) ast examples/basic.ds
	./$(BIN) check examples/basic.ds

clean:
	rm -rf build $(BIN)
