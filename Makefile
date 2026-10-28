CC ?= cc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -O2 -g
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

TARGET := msh
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

.PHONY: all clean test test-diff test-pty test-stress test-syntax test-memory sanitize format demo

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

src/%.o: src/%.c include/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: all test-diff test-pty test-stress test-syntax test-memory

test-diff: all
	python3 tests/differential.py ./$(TARGET)

test-pty: all
	python3 tests/pty_job_control.py ./$(TARGET)

test-stress: all
	python3 tests/stress.py ./$(TARGET)

test-syntax: all
	python3 tests/syntax_errors.py ./$(TARGET)

test-memory: all
	python3 tests/memory_check.py ./$(TARGET)

sanitize: clean
	$(MAKE) CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined' all

demo: all
	python3 scripts/record_demo.py ./$(TARGET) demo/msh.cast

clean:
	rm -f $(OBJ) $(TARGET)
