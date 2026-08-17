# Emerald IDE — build.
#
# Vendored dependencies: raylib 6.0 (vendor/raylib), raygui v5.0
# (vendor/raygui.h, header-only), JetBrains Mono (vendor/fonts). The raylib
# static library is built once into vendor/raylib/src/libraylib.a and reused.
#
#   make             build bin/emerald-ide
#   make headless    build the scripted session driver (no GUI needed)
#   make test        run the golden session tests (tests/*.script vs .expected)
#   make bless       regenerate the golden expectations
#   make run         build and run (opens the ray-tracer demo by default)
#   make clean       remove build artifacts

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O2 -g
INCLUDES := -Ivendor/raylib/src -Ivendor
SRCS     := src/buffer.c src/json.c src/session.c src/ui.c
HEADLESS_SRCS := headless/main.c src/buffer.c src/json.c src/session.c
RAYLIB_SRC := vendor/raylib/src
RAYLIB_LIB := $(RAYLIB_SRC)/libraylib.a
BIN      := bin/emerald-ide
HEADLESS := bin/headless

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LIBS := -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
else
  # best-effort Linux desktop (not the target platform; see SPEC.md §15)
  LIBS := -lGL -lm -lpthread -ldl -lX11
endif

all: $(BIN)

$(RAYLIB_LIB): $(RAYLIB_SRC)/raylib.h
	$(MAKE) -C $(RAYLIB_SRC) PLATFORM=PLATFORM_DESKTOP

$(BIN): $(SRCS) src/ide.h vendor/raygui.h vendor/fonts/JetBrainsMono-Regular.ttf $(RAYLIB_LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) $(RAYLIB_LIB) $(LIBS) -o $(BIN)

headless: $(HEADLESS)

$(HEADLESS): $(HEADLESS_SRCS) src/ide.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc $(HEADLESS_SRCS) -o $(HEADLESS)

# builtin.script runs with a clean environment from outside the repo so the
# sibling emeraldc cannot be found and the built-in linter takes over
define run_script
case "$1" in *builtin*) (cd /tmp && env -i $(CURDIR)/$(HEADLESS) "$(CURDIR)/$1") ;; *) ./$(HEADLESS) "$1" ;; esac
endef

test: $(HEADLESS)
	@fail=0; \
	for s in tests/*.script; do \
		name="$${s%.script}"; \
		$(call run_script,$$s) > "$$name.out" 2>&1 || true; \
		if diff -u "$$name.expected" "$$name.out" > /tmp/eide_diff.$$$$ 2>&1; then \
			echo "PASS $$name"; \
		else \
			echo "FAIL $$name"; cat /tmp/eide_diff.$$$$; fail=1; \
		fi; \
		rm -f /tmp/eide_diff.$$$$; \
	done; \
	exit $$fail

bless: $(HEADLESS)
	@for s in tests/*.script; do \
		name="$${s%.script}"; \
		$(call run_script,$$s) > "$$name.expected" 2>&1 || true; \
		echo "blessed $$name"; \
	done

run: $(BIN)
	./$(BIN)

clean:
	rm -rf bin
	$(MAKE) -C $(RAYLIB_SRC) clean

.PHONY: all headless test bless run clean
