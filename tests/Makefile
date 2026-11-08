# Unit tests. Builds the library sources (everything except main.c) into the
# test binary, so tests exercise the real code paths, not copies.
#
#   make -C tests run          build and run
#   make -C tests run SAN=1    with ASan/UBSan

CC     ?= cc
CSTD   := -std=c11
WARN   := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
DEFS   := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS ?= -O1 -g
LDLIBS := -lm -lpthread -ldl

SRC    := ../src
LIB_SRCS := $(filter-out $(SRC)/main.c,$(wildcard $(SRC)/*.c))
OBJS   := $(patsubst $(SRC)/%.c,%.o,$(LIB_SRCS))
TEST_OBJS := test_main.o

ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

run: run.bin
	./run.bin

run.bin: $(OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) $(CSTD) -o $@ $(OBJS) $(TEST_OBJS) $(LDLIBS)

%.o: $(SRC)/%.c
	$(CC) $(CSTD) $(WARN) $(DEFS) $(CFLAGS) -I$(SRC) -c $< -o $@

test_main.o: test_main.c test.h
	$(CC) $(CSTD) $(WARN) $(DEFS) $(CFLAGS) -I$(SRC) -c $< -o $@

clean:
	rm -f *.o run.bin

.PHONY: run clean
