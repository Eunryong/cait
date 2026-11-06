CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

SRCS := $(filter-out src/gui_stub.c,$(wildcard src/*.c))

ifeq ($(shell uname),Darwin)
GUI := src/gui.m src/gui_markdown.m
GUIFLAGS := -fobjc-arc -framework Cocoa -framework WebKit
else
GUI := src/gui_stub.c
GUIFLAGS :=
endif

cait: $(SRCS) $(GUI) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(GUI) $(GUIFLAGS)

clean:
	rm -f cait

test: cait
	tests/run.sh

.PHONY: clean test
