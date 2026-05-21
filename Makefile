CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -g
CPPFLAGS ?= -Iinclude

SRC := src/main.c src/cli_program.c src/source.c src/diag.c src/lexer.c src/ast.c src/parser.c src/parse_expr.c src/parse_command.c src/parse_script.c src/parse_function.c src/parse_stmt.c src/lower.c src/lower_symbols.c src/lower_expr.c src/lower_stmt.c src/lower_stdlib.c src/lower_functions.c src/lower_tests.c src/lower_free.c src/hir.c src/format.c src/checker.c src/command.c src/runtime.c src/runtime/hashmap.c src/stdlib.c src/vm.c src/vm_args.c src/vm_compile.c src/vm_dump.c src/vm_process.c src/vm_scope.c src/vm_stdlib.c src/vm_test.c src/bash_helpers.c src/bash_quote.c src/bash_expr.c src/bash_command.c src/bash_deps.c src/bash_stmt.c src/bash_emit.c
OBJ := $(SRC:src/%.c=build/%.o)
PRIVATE_HEADERS := $(wildcard src/*.h src/runtime/*.h)
BIN := ds
TEST_VERSIONS := 0-1 0-2 0-3 0-4 0-5 0-6 0-7 0-8 0-9 0-10 0-11 0-12 0-13 0-14 0-15 0-16 0-17 0-18 0-19 0-20
TEST_TARGETS := $(addprefix test-v,$(TEST_VERSIONS))

.PHONY: all clean check smoke test $(TEST_TARGETS) asan ubsan

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

build/%.o: src/%.c include/ds.h $(PRIVATE_HEADERS) | build
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

check: $(BIN)
	./$(BIN) check examples/basic.ds
	! ./$(BIN) check examples/bad.ds >/tmp/ds_bad.out 2>&1

test: $(BIN)
	@for version in $(TEST_VERSIONS); do \
		dir=$$(printf '%s' "$$version" | tr '-' '_'); \
		DS_SKIP_BUILD=1 ./tests/v$$dir/run.sh || exit $$?; \
	done

$(TEST_TARGETS): $(BIN)
	DS_SKIP_BUILD=1 ./tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" all
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 DS_SKIP_BUILD=1 $(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test

ubsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" all
	DS_SKIP_BUILD=1 $(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test

smoke: $(BIN)
	./$(BIN) tokens examples/basic.ds
	./$(BIN) ast examples/basic.ds
	./$(BIN) check examples/basic.ds

clean:
	rm -rf build $(BIN)
