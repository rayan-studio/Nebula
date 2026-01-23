#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import shutil
import subprocess
from pathlib import Path
from datetime import datetime

# ============================================================
# Colors (works in Windows Terminal / modern consoles)
# ============================================================
class C:
    R   = "\033[0m"
    B   = "\033[1m"
    D   = "\033[2m"
    G   = "\033[32m"
    Y   = "\033[33m"
    RED = "\033[31m"
    CY  = "\033[36m"

if sys.platform == "win32":
    os.system("")  # enable ANSI on modern Windows consoles

def log(msg, color=""):
    print(f"{color}{msg}{C.R}")

# ============================================================
# Paths
# ============================================================
ROOT  = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"

# Default config
DEFAULT_CONFIG = "Release"

# ============================================================
# Helpers
# ============================================================
def run(cmd, cwd=None, capture=False):
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=capture
    )

def run_stream(cmd, cwd=None, filter_fn=None):
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1
    )
    for line in proc.stdout:
        line = line.rstrip("\r\n")
        if filter_fn:
            out = filter_fn(line)
            if out:
                print(out)
        else:
            print(line)
    proc.wait()
    return proc.returncode

def parse_msbuild(line: str):
    # Skip noise
    skip = [
        "Version MSBuild",
        "Checking File Globs",
        "Checking Build System",
        "Building Custom Rule",
        "Génération de code",
        "compiler le fichier source",
        "with [",
        "0 Warning(s)",
        "0 Error(s)",
    ]
    if any(s in line for s in skip):
        return None

    # VS project output lines
    if ".vcxproj ->" in line:
        m = re.search(r"(\w+)\.vcxproj -> .*[/\\]([\w\.\-]+)$", line)
        if m:
            project, output = m.groups()
            return f"{C.D}→{C.R} {project} {C.D}→{C.R} {output}"

    # Compilation progress lines
    if re.match(r"^\s+\w+\.(cpp|c|cc|cxx)$", line):
        return f"{C.D}[building]{C.R} {line.strip()}"

    # Errors
    if "error" in line.lower():
        return f"{C.RED}✗{C.R} {line}"

    # Warnings
    if "warning" in line.lower():
        m = re.search(r"\bwarning\s+(C\d+)\b", line)
        if m:
            return f"{C.Y}⚠{C.R} {m.group(1)}"
        return f"{C.Y}⚠{C.R} {line}"

    return None

def cmake_generators_text():
    r = run(["cmake", "--help"], capture=True)
    if r.returncode != 0:
        return ""
    return r.stdout

def has_generator(help_text: str, name: str) -> bool:
    # Look for generator in cmake --help output
    # It usually appears in the "Generators" section.
    return name.lower() in help_text.lower()

def pick_generator():
    help_text = cmake_generators_text()

    # Preference order:
    # 1) VS 2026 if available
    # 2) VS 2022 if available
    # 3) Ninja if available
    if has_generator(help_text, "Visual Studio 18 2026"):
        return ("Visual Studio 18 2026", ["-G", "Visual Studio 18 2026", "-A", "x64"], True)
    if has_generator(help_text, "Visual Studio 17 2022"):
        return ("Visual Studio 17 2022", ["-G", "Visual Studio 17 2022", "-A", "x64"], True)
    if has_generator(help_text, "Ninja"):
        return ("Ninja", ["-G", "Ninja"], False)

    # If nothing found, let CMake decide (rare)
    return ("(default)", [], False)

def exe_path_for(config: str) -> Path:
    # With Visual Studio multi-config: build/Release/Nebula.exe
    # With Ninja single-config: build/Nebula.exe (or build/src/... depending project)
    # Your CMake adds_executable(Nebula WIN32 ...), so usually it's in build/<config>/Nebula.exe for VS
    p1 = BUILD / config / "Nebula.exe"
    p2 = BUILD / "Nebula.exe"
    return p1 if p1.exists() else p2

# ============================================================
# Main
# ============================================================
def main():
    args = [a.lower() for a in sys.argv[1:]]

    # Commands:
    #   python scripts/build.py
    #   python scripts/build.py clean
    #   python scripts/build.py debug
    #   python scripts/build.py release
    if "clean" in args or "--clean" in args or "-c" in args:
        log(f"{C.D}→ cleaning {BUILD}{C.R}")
        shutil.rmtree(BUILD, ignore_errors=True)
        log(f"{C.G}✓ done{C.R}")
        return 0

    config = DEFAULT_CONFIG
    if "debug" in args:
        config = "Debug"
    if "release" in args:
        config = "Release"

    log(f"{C.B}nebula{C.R} {C.D}cmake builder{C.R}")
    log(f"{C.D}{datetime.now().strftime('%H:%M:%S')}{C.R}\n")

    BUILD.mkdir(exist_ok=True)

    gen_name, gen_args, is_multi_config = pick_generator()
    log(f"{C.D}→ generator: {C.CY}{gen_name}{C.R}")

    # Configure
    log(f"{C.D}→ configuring{C.R}")
    r = run(["cmake", "-S", str(ROOT), "-B", str(BUILD), *gen_args,
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], capture=True)
    if r.returncode != 0:
        log(f"{C.RED}✗ cmake configuration failed{C.R}\n")
        print(r.stdout)
        print(r.stderr)
        return r.returncode

    # Print useful config lines
    for line in (r.stdout or "").splitlines():
        if "Windows SDK" in line or "Using" in line or "libvterm" in line or "ggwave" in line:
            log(f"{C.D}  {line.strip()}{C.R}")

    # Build
    log(f"\n{C.D}→ building ({config.lower()}){C.R}")
    build_cmd = ["cmake", "--build", str(BUILD)]
    if is_multi_config:
        build_cmd += ["--config", config]

    ret = run_stream(build_cmd, cwd=BUILD, filter_fn=parse_msbuild)
    if ret != 0:
        log(f"\n{C.RED}✗ build failed{C.R}")
        return ret
    # Success
    exe = exe_path_for(config)
    if exe.exists():
        size_kb = exe.stat().st_size / 1024
        log(f"\n{C.G}✓{C.R} {exe.name} {C.D}({size_kb:.1f} kb){C.R}")
        log(f"{C.D}  {exe.resolve()}{C.R}\n")
        return 0

    log(f"\n{C.Y}⚠ build completed but Nebula.exe not found{C.R}")
    log(f"{C.D}  searched: {BUILD / config / 'Nebula.exe'} and {BUILD / 'Nebula.exe'}{C.R}\n")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        log(f"\n{C.Y}⚠ interrupted{C.R}\n")
        raise SystemExit(1)
    except Exception as e:
        log(f"\n{C.RED}✗ {e}{C.R}\n")
        raise SystemExit(1)
