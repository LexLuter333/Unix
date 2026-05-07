
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -pedantic -std=c11 -O2


TARGET  := myinit
SRC     := myinit.c
TEST    := runme.sh
LOG     := /tmp/myinit.log

CONFIG_FILE ?=


.PHONY: all build test run clean help

all: build

build: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

test: build
	@if [ ! -x "$(TEST)" ]; then chmod +x $(TEST); fi

run: build
	@if [ -z "$(CONFIG_FILE)" ]; then \
		exit 1; \
	fi
	@./$(TARGET) $(CONFIG_FILE)


clean:

	@rm -f $(TARGET)
	@rm -f $(LOG)
	@rm -rf /tmp/myinit_test_*
	@rm -f /tmp/in* /tmp/out* /tmp/test*.log


