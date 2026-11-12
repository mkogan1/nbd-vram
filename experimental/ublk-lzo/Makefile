UBLK_SRC ?=
CUDA_INCLUDE ?= /opt/cuda/include
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
UBLK_ARCHIVE = $(UBLK_SRC)/lib/.libs/libublksrv.a

ublk-lzo-target: Makefile ublk-lzo-target.c cuda-vmm-pool.c nbd-vram.c $(wildcard *.h) $(if $(UBLK_SRC),$(UBLK_ARCHIVE))
	@test -n "$(UBLK_SRC)" || { echo 'UBLK_SRC is required; set it to the libublksrv source/build directory' >&2; exit 1; }
	@test -f "$(UBLK_ARCHIVE)" || { echo "libublksrv archive not found: $(UBLK_ARCHIVE)" >&2; exit 1; }
	$(CC) $(CPPFLAGS) $(CFLAGS) -I"$(CUDA_INCLUDE)" -I"$(UBLK_SRC)/include" -I"$(UBLK_SRC)" $(LDFLAGS) -o $@ ublk-lzo-target.c cuda-vmm-pool.c "$(UBLK_ARCHIVE)" -luring -lstdc++ -ldl -pthread

.PHONY: stage check-stage check-cpu check-sanitizers
check-sanitizers:
	UBLK_SRC="$(UBLK_SRC)" CUDA_INCLUDE="$(CUDA_INCLUDE)" CC="$(CC)" bash ./test-vmm-sanitizers.sh

check-cpu:
	@test -n "$(UBLK_SRC)" || { echo 'UBLK_SRC is required; set it to the libublksrv source/build directory' >&2; exit 1; }
	UBLK_SRC="$(UBLK_SRC)" CUDA_INCLUDE="$(CUDA_INCLUDE)" CC="$(CC)" bash ./test-experimental-cpu.sh

check-stage: ublk-lzo-target
	python3 test-ublk-stage.py

# Require an explicit staging root; this target never installs on the live host.
stage: ublk-lzo-target
	@test -n "$(DESTDIR)" && test "$$(realpath -m -- "$(DESTDIR)")" != / || { echo 'DESTDIR must name a non-root staging directory' >&2; exit 1; }
	install -D -m 0755 ublk-lzo-target "$(DESTDIR)/usr/local/libexec/vram-swap/ublk-lzo-target"
	install -D -m 0755 vram-swap-ublk-sleep.sh "$(DESTDIR)/usr/local/libexec/vram-swap/vram-swap-ublk-sleep.sh"
	install -D -m 0644 vram-swap-ublk.service "$(DESTDIR)/etc/systemd/system/vram-swap-ublk.service"
	install -D -m 0644 vram-swap-ublk-suspend.service "$(DESTDIR)/etc/systemd/system/vram-swap-ublk-suspend.service"
	install -D -m 0644 vram-swap-ublk.modules-load.conf "$(DESTDIR)/etc/modules-load.d/vram-swap-ublk.conf"
	install -D -m 0644 vram-swap-ublk.conf.example "$(DESTDIR)/usr/share/doc/vram-swap-ublk/vram-swap-ublk.conf.example"
