CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -g
CPPFLAGS ?= -Iinclude
include config/feature_flags.mk
CPPFLAGS += $(DS_FEATURE_CPPFLAGS)

SRC := src/main.c src/cli_program.c src/ds_common.c src/source.c src/diag.c src/lexer.c src/ast.c src/parser.c src/parse_expr.c src/parse_command.c src/parse_script.c src/parse_function.c src/parse_stmt.c src/lower.c src/lower_symbols.c src/lower_expr.c src/lower_interpolation.c src/lower_collection.c src/lower_command.c src/lower_stmt.c src/lower_stdlib.c src/lower_functions.c src/lower_free.c src/hir.c src/format.c src/ds_checker.c src/ds_command.c src/ds_command_facts.c src/ds_interpolation.c src/ds_signal.c src/runtime.c src/runtime/hashmap.c src/ds_stdlib.c src/ds_regex.c src/vm.c src/vm_args.c src/vm_compile.c src/vm_dump.c src/vm_process.c src/vm_scope.c src/vm_stdlib.c src/bash_helpers.c src/bash_quote.c src/bash_structured.c src/bash_expr.c src/bash_command.c src/bash_function.c src/bash_deps.c src/bash_stmt.c src/bash_emit.c
OBJ := $(SRC:src/%.c=build/%.o)
PRIVATE_HEADERS := $(wildcard src/*.h src/*.def src/generated/*.inc src/runtime/*.h)
PROJECT_HEADERS := $(wildcard include/*.h src/*.h src/runtime/*.h)
NODE_GENERATOR := build/gen_nodes
NODE_SCHEMAS := src/ast_nodes.def src/hir_nodes.def
NODE_GENERATED := src/generated/ast_expr_kinds.inc src/generated/ast_expr_union.inc src/generated/ast_expr_free.inc \
	src/generated/ast_stmt_kinds.inc src/generated/ast_stmt_union.inc src/generated/ast_stmt_free.inc \
	src/generated/hir_expr_kinds.inc src/generated/hir_expr_union.inc src/generated/hir_expr_free.inc \
	src/generated/hir_stmt_kinds.inc src/generated/hir_stmt_union.inc src/generated/hir_stmt_free.inc
BIN := ds
TEST_VERSIONS := 0-1 0-2 0-3 0-4 0-5 0-6 0-7 0-8 0-9 0-10 0-11 0-12 0-13 0-14 0-15 0-16 0-17 0-18 0-19 0-20 0-21 0-22 0-23 0-24 0-25 0-26 0-27 0-29 0-30 0-31 0-32 0-33 0-34 0-35 0-36 0-37 0-38
TEST_TARGETS := $(addprefix test-v,$(TEST_VERSIONS))

.PHONY: all clean generate check-generated check check-compile-flags check-header-boundaries smoke test $(TEST_TARGETS) test-v0-22-signal-runtime asan ubsan test-asan test-ubsan

all: $(BIN)

generate: $(NODE_GENERATOR)
	./$(NODE_GENERATOR)

check-generated: $(NODE_GENERATOR)
	@tmp=$$(mktemp -d); \
	./$(NODE_GENERATOR) --output-dir "$$tmp"; \
	status=0; \
	diff -ru --exclude='.stamp' src/generated "$$tmp" || status=$$?; \
	rm -rf "$$tmp"; \
	exit $$status

$(NODE_GENERATOR): tools/gen_nodes.c | build
	$(CC) $(CFLAGS) $< -o $@

src/generated/.stamp: $(NODE_GENERATOR) $(NODE_SCHEMAS)
	./$(NODE_GENERATOR)
	@touch $@

$(NODE_GENERATED): src/generated/.stamp

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

build/%.o: src/%.c include/ds.h $(PRIVATE_HEADERS) $(NODE_GENERATED) | build
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

check: check-generated check-compile-flags check-header-boundaries $(BIN)
	./$(BIN) check examples/basic.ds
	! ./$(BIN) check examples/bad.ds >/tmp/ds_bad.out 2>&1

check-compile-flags:
	@for flag in $(DS_FEATURE_CPPFLAGS); do \
		grep -Fqx -- "$$flag" compile_flags.txt || { \
			echo "compile_flags.txt is missing $$flag" >&2; \
			exit 1; \
		}; \
	done

check-header-boundaries:
	@if grep -nE '^[[:space:]]*(static[[:space:]]+)?([[:alnum:]_]+[[:space:]*]+)+[[:alnum:]_]+\([^;{}]*\)[[:space:]]*\{' $(PROJECT_HEADERS); then \
		echo "project headers must not contain function implementations" >&2; \
		exit 1; \
	fi
	@bad_macros=$$(grep -hE '^#define [A-Za-z0-9_]+.*\\$$' $(PROJECT_HEADERS) | \
		sed -E 's/^#define ([A-Za-z0-9_]+).*/\1/' | \
		grep -vE '^(DS_VEC_PUSH|DS_VM_OPCODE_LIST)$$' || true); \
	if [ -n "$$bad_macros" ]; then \
		echo "unexpected multiline implementation macros in project headers: $$bad_macros" >&2; \
		exit 1; \
	fi

test: $(BIN)
	@for version in $(TEST_VERSIONS); do \
		dir=$$(printf '%s' "$$version" | tr '-' '_'); \
		DS_SKIP_BUILD=1 ./tests/v$$dir/run.sh || exit $$?; \
	done

$(TEST_TARGETS): $(BIN)
	DS_SKIP_BUILD=1 ./tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh

test-v0-22-signal-runtime: $(BIN)
	DS_SKIP_BUILD=1 ./tests/v0_22/signal_runtime.sh

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" all
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 DS_SKIP_BUILD=1 $(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test

test-asan: asan

ubsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" all
	DS_SKIP_BUILD=1 $(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test

test-ubsan: ubsan

smoke: $(BIN)
	./$(BIN) tokens examples/basic.ds
	./$(BIN) ast examples/basic.ds
	./$(BIN) check examples/basic.ds

clean:
	rm -rf build $(BIN) src/generated/.stamp
