"""Exercise installer choices and status rendering without touching live swap."""
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent


def shell(script, stdin="", env=None):
    return subprocess.run(
        ["bash", "-c", "set -e\n" + script], input=stdin, text=True,
        capture_output=True, check=True, timeout=10, env={**os.environ, **(env or {})},
    ).stdout


with tempfile.TemporaryDirectory(prefix="nbd-vram-script-test-") as directory:
    unit = Path(directory) / "test.service"
    installed = "/etc/systemd/system/vram-swap-nbd.service"
    installer = (ROOT / "install.sh").read_text().replace(installed, str(unit))
    start = installer.index("    COMPRESS_DEFAULT=")
    end = installer.index("\nelse\n    # Non-interactive reinstall", start)
    choices = installer[start:end]
    stubs = '''
ldconfig() { printf 'liblz4.so.1\\nlibzstd.so.1\\n'; }
apt-get() { echo 'unexpected package installation' >&2; return 1; }
ALLOC=3072
'''
    # All combinations, including legacy defaults and a rejected dedup choice.
    for previous, stdin, codec, dedup in (
        ({}, "off\noff\n", "off", "0"),
        ({}, "off\non\n2.5\n", "off", "1"),
        ({}, "lz4\non\n2.5\n", "lz4", "1"),
        ({"PREV_COMPRESS": "0"}, "\noff\n", "off", "0"),
        ({}, "0\noff\n", "off", "0"),
        ({}, "zstd:9\noff\n2.5\n", "zstd:9", "0"),
        ({"PREV_COMPRESS": "1", "PREV_DEDUP": "1"}, "\n\n2.5\n", "lz4", "1"),
        ({"PREV_COMPRESS": "zstd:3", "PREV_DEDUP": "0"}, "\nmaybe\non\n2.5\n", "zstd:3", "1"),
    ):
        unit.write_text((ROOT / "systemd/vram-swap-nbd.service").read_text())
        env = {"PREV_COMPRESS": "", "PREV_DEDUP": "", "PREV_RATIO": "", **previous}
        output = shell(stubs + choices, stdin, env)
        config = unit.read_text()
        assert f"Environment=VRAM_COMPRESS={codec}\n" in config
        assert f"Environment=VRAM_DEDUP={dedup}\n" in config
        assert "Page deduplication (off, on)" in output
        if codec != "off" or dedup == "1":
            assert "Environment=VRAM_COMPRESS_RATIO=2.5\n" in config
        else:
            assert "Logical size / VRAM ratio" not in output

    # Non-interactive reinstalls retain both on and off from the installed unit.
    previous_lines = "\n".join(line for line in installer.splitlines() if line.startswith("PREV_"))
    preserve_start = installer.index("    # Non-interactive reinstall")
    preserve_end = installer.index("\nfi\n", preserve_start)
    for previous, codec, dedup in (("zstd:9", "zstd:9", "0"), ("zstd:9", "zstd:9", "1"), ("0", "off", "0"), ("1", "lz4", "1")):
        unit.write_text(f"Environment=VRAM_COMPRESS={previous}\nEnvironment=VRAM_DEDUP={dedup}\n")
        reset = f"cp '{ROOT / 'systemd/vram-swap-nbd.service'}' '{unit}'\n"
        shell(previous_lines + "\n" + reset + installer[preserve_start:preserve_end])
        assert f"Environment=VRAM_DEDUP={dedup}\n" in unit.read_text()
        assert f"Environment=VRAM_COMPRESS={codec}\n" in unit.read_text()

    status_path = Path(directory) / "status"
    status_script = (ROOT / "nbd-vram-compression-status.sh").read_text().replace(
        "STATUS=/run/nbd-vram.status", f"STATUS={status_path}"
    )
    base = """configured_ratio=2.0
configured_ratio_tenths=20
vram_bytes=1048576
export_bytes=2097152
vram_slab_bytes=65536
vram_obj_bytes=4096
pages_same=1
enospc=0
"""
    for codec, compress in (("off", "0"), ("lz4", "1"), ("zstd", "1")):
        status_path.write_text(base + f"""compress={compress}
algorithm={codec}
compression_level=3
dedup=1
dedup_pages=2
dedup_unique_pages=1
dedup_saved_bytes=8192
dedup_hits=5
dedup_index_bytes=1024
pages_lz4={3 if codec == 'lz4' else 0}
pages_zstd={3 if codec == 'zstd' else 0}
pages_raw={3 if codec == 'off' else 0}
""")
        output = shell(status_script)
        assert "pages            : 4 " in output
        assert "deduped pages    : 2 extra copies avoided (1 unique payloads)" in output
        assert "dedup savings    : 8192 bytes (0.01 MiB of object slots)" in output
        assert "dedup matches    : 5 matching page writes since start" in output
        assert "dedup index RAM  : 1024 bytes" in output

    # Older daemons omit all new fields.
    status_path.write_text(base + "compress=1\npages_lz4=3\npages_raw=0\n")
    output = shell(status_script)
    assert "pages            : 4 " in output and "deduplication    : off" in output

    # Exercise the discard predicates used by all connection/test helpers.
    for name in ("nbd-vram-connect.sh", "test-nbd.sh", "test-fill.sh"):
        script = (ROOT / name).read_text()
        conditions = re.findall(r'if (\[ "\$\{VRAM_COMPRESS[^\n]+); then', script)
        assert conditions
        for condition in conditions:
            for codec, dedup in (("off", "0"), ("off", "1"), ("0", "0"), ("0", "1"), ("1", "0"), ("lz4", "0"), ("zstd:3", "1")):
                output = shell(f'if {condition}; then echo discard; else echo direct; fi',
                               env={"VRAM_COMPRESS": codec, "VRAM_DEDUP": dedup})
                assert output.strip() == ("direct" if codec in ("off", "0") and dedup == "0" else "discard")

print("PASS installer defaults/preservation, dedup stats, legacy status, and discard activation")
