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

# Drives the shipping helper_main() with random, boundary and truncated
# AudioSocket frames and odd PCM bursts. Most useful under sanitizers:
#   make clean fuzz CFLAGS='-O1 -g -fsanitize=address,undefined -pthread' \
#                   LDFLAGS='-fsanitize=address,undefined -pthread -lm'
fuzz_helper: tests/fuzz_helper.c slmbridge.c
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ tests/fuzz_helper.c $(LDFLAGS)

fuzz: fuzz_helper
	./fuzz_helper

check: slmbridge_rstest fuzz_helper
	./slmbridge_rstest
	./fuzz_helper 100

clean:
	rm -f slmbridge slmbridge_rstest fuzz_helper

.PHONY: all check clean fuzz
