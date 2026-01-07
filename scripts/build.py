#!/usr/bin/env python3
"""
build.py — lightweight wrapper to configure+build the project via CMake

Features:
- Uses `rich` for pretty, modern logs if available (falls back to simple ANSI logging).
- Configures a `build/` directory, runs CMake configure and build steps.
- Streams subprocess output live into the console.

Usage:
    python scripts\build.py [--generator "Visual Studio 17 2022"] [--arch x64] [--config Release]

"""
from __future__ import annotations
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.live import Live
    from rich.text import Text
    RICH_AVAILABLE = True
except Exception:
    RICH_AVAILABLE = False


def run(cmd, cwd=None, console=None, live=None):
    """Run command and stream stdout/stderr."""
    if console is None:
        console = Console() if RICH_AVAILABLE else None

    process = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, shell=False)
    assert process.stdout is not None
    for line in process.stdout:
        line = line.rstrip("\n")
        if RICH_AVAILABLE and console:
            console.print(line)
        else:
            print(line)
    process.wait()
    return process.returncode


def detect_generator() -> str:
    # Try to detect an available CMake generator by parsing `cmake --help` output.
    candidates = [
        "Visual Studio 17 2022",
        "Visual Studio 16 2019",
        "Ninja",
        "NMake Makefiles",
        "MinGW Makefiles",
    ]
    try:
        p = subprocess.run(["cmake", "--help"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=True)
        out = p.stdout
        for c in candidates:
            if c in out:
                return c
    except Exception:
        pass
    return "Visual Studio 17 2022"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate and build the Nebula project with CMake (pretty logs)")
    parser.add_argument("--generator", default=None, help="CMake generator")
    parser.add_argument("--arch", default="x64", help="Architecture for generator (e.g. x64)")
    parser.add_argument("--config", default="Release", help="Build configuration")
    parser.add_argument("--build-dir", default="build", help="Build directory")
    parser.add_argument("--no-configure", action="store_true", help="Skip CMake configure step")
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parents[1]
    build_dir = project_root / args.build_dir

    console = Console() if RICH_AVAILABLE else None
    if RICH_AVAILABLE:
        console.print(Panel.fit(Text("Nebula CMake Builder", style="bold white on blue")))
    else:
        print("== Nebula CMake Builder ==")

    if not build_dir.exists():
        build_dir.mkdir(parents=True, exist_ok=True)

    # Configure step
    if not args.no_configure:
        generator = args.generator or os.environ.get("CMAKE_GENERATOR") or detect_generator()
        cmake_cmd = [
            "cmake",
            str(project_root),
            "-G",
            generator,
        ]
        # Only pass -A when the generator supports architectures (Visual Studio)
        if args.arch and "Visual Studio" in generator:
            cmake_cmd += ["-A", args.arch]
        if RICH_AVAILABLE:
            console.print(f"[cyan]Configuring with:[/cyan] {cmake_cmd}")
        else:
            print("Configuring:", " ".join(cmake_cmd))

        rc = run(cmake_cmd, cwd=str(build_dir), console=console)
        if rc != 0:
            if RICH_AVAILABLE:
                console.print(Panel(Text("CMake configure failed", style="bold red")))
            else:
                print("CMake configure failed")
            return rc

    # Build step
    build_cmd = [
        "cmake",
        "--build",
        ".",
        "--config",
        args.config,
    ]
    if RICH_AVAILABLE:
        console.print(f"[green]Building ({args.config})...[/green]")
    else:
        print("Building:", " ".join(build_cmd))

    rc = run(build_cmd, cwd=str(build_dir), console=console)
    if rc != 0:
        if RICH_AVAILABLE:
            console.print(Panel(Text("Build failed", style="bold red")))
        else:
            print("Build failed")
        return rc

    if RICH_AVAILABLE:
        console.print(Panel(Text("Build succeeded", style="bold green")))
    else:
        print("Build succeeded")

    return 0


if __name__ == "__main__":
    sys.exit(main())
