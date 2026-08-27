CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread
LDFLAGS ?= -pthread -lm

all: slmbridge

slmbridge: slmbridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Includes slmbridge.c rather than linking against it, so the test exercises
# the shipping resampler instead of a copy that can drift out of sync with it.
slmbridge_rstest: slmbridge_rstest.c slmbridge.c
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $< $(LDFLAGS)

check: slmbridge_rstest
	./slmbridge_rstest

clean:
	rm -f slmbridge slmbridge_rstest

.PHONY: all check clean
