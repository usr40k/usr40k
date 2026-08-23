#!/usr/bin/env python3
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str]) -> None:
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        raise SystemExit(f"Command failed: {' '.join(cmd)}")
    if result.stdout:
        print(result.stdout.rstrip())


def write_file(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")


def compile_wasm(root: Path, out_dir: Path) -> None:
    """Compile shell.c to shell.wasm using clang targeting wasm32."""
    src = root / "wasm-shell" / "shell.c"
    out = out_dir / "shell.wasm"

    if not src.exists():
        raise SystemExit(f"Source file not found: {src}")

    clang = shutil.which("clang")
    if not clang:
        raise SystemExit(
            "clang not found. Install clang and lld to build the WASM shell.\n"
            "  openSUSE: sudo zypper install clang lld\n"
            "  Debian/Ubuntu: sudo apt install clang lld\n"
            "  Fedora: sudo dnf install clang lld"
        )

    cmd = [
        clang,
        "--target=wasm32",
        "-O2",
        "-nostdlib",
        "-fno-builtin",
        "-Wl,--no-entry",
        "-Wl,--export-all",
        "-Wl,--allow-undefined",
        "-Wl,--initial-memory=2097152",
        "-Wl,--max-memory=2097152",
        "-o",
        str(out),
        str(src),
    ]

    print("Compiling WASM shell...")
    run(cmd)

    size = out.stat().st_size
    print(f"  -> {out} ({size:,} bytes)")


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    out_dir = root / "wasm-shell"

    dumshrc = """
alias ll='ls -al'
alias la='ls -A'
alias l='ls -CF'

PROMPT='usr40k@sys:%~$ '
"""

    

    manifest = {
        "name": "usr40k-web-shell",
        "runtime": "wasm",
        "shell": "dumsh",
        "target": "browser",
        "version": "1.0.0",
        "wasm": {
            "module": "shell.wasm",
            "runtime": "shell-runtime.js",
            "memory": "2MiB",
            "compiler": "clang --target=wasm32",
        },
        "files": [
            ".dumshrc",
            "manifest.json",
            "shell.c",
            "shell.wasm",
            "shell-runtime.js",
            "test-wasm.js",
        ],
    }

    write_file(out_dir / ".dumshrc", dumshrc)
    
    
    write_file(out_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")

    # Compile the WASM module
    compile_wasm(root, out_dir)

    print(f"Generated WASM shell package at: {out_dir}")


if __name__ == "__main__":
    main()
