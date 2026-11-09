# TermiPlayer — a terminal media player
#
# Zero build-time dependencies beyond a C11 compiler and libc.
# Optional libraries (zlib for kitty compression, libpulse for audio output)
# are loaded at runtime with dlopen() and degrade gracefully when absent.
#
#   make            build ./player (optimised)
#   make debug      build with -O0 -g -fsanitize=address,undefined
#   make test       build and run the test suite
#   make fixtures   generate test media (requires ffmpeg on PATH)
#   make clean

CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -Wpointer-arith -Wno-unused-parameter
DEFS    := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64

SRC_DIR := src
BUILD   := build
SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

CFLAGS  ?= -O2 -g
LDLIBS  := -lm -lpthread -ldl

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

TARGET  := player

.PHONY: all debug test test-san fixtures clean install fmt

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(CSTD) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(DEFS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer" all

test: all
	@$(MAKE) -s -C tests run
	@sh tests/integration.sh

# Sanitizer build of the whole binary plus both suites.
test-san: clean
	@$(MAKE) -s CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" all
	@$(MAKE) -s -C tests clean
	@$(MAKE) -s -C tests run SAN=1
	@sh tests/integration.sh

fixtures:
	@sh tools/mkfixtures.sh

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET) tests/run tests/run.bin tests/*.o tests/fixtures/*.tmp

-include $(DEPS)
