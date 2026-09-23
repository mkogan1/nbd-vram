all: nbd-vram

nbd-vram: nbd-vram.c
	gcc -O2 -Wall -o nbd-vram nbd-vram.c -ldl -lpthread

clean:
	rm -f nbd-vram

install: nbd-vram
	@echo "Use install.sh for full installation"

# Real codec libraries, simulated CUDA memory; no root, GPU, or active swap needed.
.PHONY: test
test:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) -O2 -g -Wall -Wextra -Werror $(CFLAGS) -o "$$tmp/test-compression" test-compression.c -ldl -lpthread; \
	for codec in 1 lz4 zstd zstd:1 zstd:9 zstd:22; do "$$tmp/test-compression" "$$codec"; done
