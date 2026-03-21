#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import shutil
import subprocess
from pathlib import Path
from datetime import datetime


class C:
    R = "\033[0m"
    B = "\033[1m"
    D = "\033[2m"
    G = "\033[32m"
    Y = "\033[33m"
    RED = "\033[31m"
    CY = "\033[36m"


if sys.platform == "win32":
    os.system("")


def log(msg, color=""):
    print(f"{color}{msg}{C.R}")


ROOT = Path(__file__).resolve().parent.parent
BUILD_DEFAULT = ROOT / "build"
BUILD_FAST = ROOT / "build-ninja"
DEFAULT_CONFIG = "Release"
EXTERNAL_DEPS = [
    {
        "name": "libvterm",
        "path": ROOT / "external" / "libvterm",
        "marker": ROOT / "external" / "libvterm" / "include" / "vterm.h",
        "url": "https://github.com/neovim/libvterm.git",
    },
    {
        "name": "ggwave",
        "path": ROOT / "external" / "ggwave",
        "marker": ROOT / "external" / "ggwave" / "include" / "ggwave" / "ggwave.h",
        "url": "https://github.com/ggerganov/ggwave.git",
    },
    {
        "name": "nanosvg",
        "path": ROOT / "external" / "nanosvg",
        "marker": ROOT / "external" / "nanosvg" / "src" / "nanosvg.h",
        "url": "https://github.com/memononen/nanosvg.git",
    },
    {
        "name": "libgit2",
        "path": ROOT / "external" / "libgit2",
        "marker": ROOT / "external" / "libgit2" / "include" / "git2.h",
        "url": "https://github.com/libgit2/libgit2.git",
    },
]


def run(cmd, cwd=None, capture=False):
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=capture,
    )


def run_stream(cmd, cwd=None, filter_fn=None, collect=False):
    collected = []
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    for line in proc.stdout:
        line = line.rstrip("\r\n")
        if collect:
            collected.append(line)
        if filter_fn:
            out = filter_fn(line)
            if out:
                print(out)
        else:
            print(line)
    proc.wait()
    if collect:
        return proc.returncode, collected
    return proc.returncode


def has_parallel_lock_error(lines):
    for line in lines:
        low = line.lower()
        if "error c1041" in low:
            return True
        if "msb6003" in low and ("en cours d'utilisation" in low or "in use by another process" in low):
            return True
        if "error c1083" in low and ".obj" in low and (
            "permission denied" in low
            or "acces refuse" in low
            or "accès refusé" in low
            or "in use by another process" in low
        ):
            return True
    return False


def parse_build_output(line: str):
    skip = [
        "Version MSBuild",
        "Checking File Globs",
        "Checking Build System",
        "Building Custom Rule",
        "Generation de code",
        "compiler le fichier source",
        "with [",
        "0 Warning(s)",
        "0 Error(s)",
    ]
    if any(s in line for s in skip):
        return None

    if ".vcxproj ->" in line:
        m = re.search(r"(\w+)\.vcxproj -> .*[/\\]([\w\.\-]+)$", line)
        if m:
            project, output = m.groups()
            return f"{C.D}->{C.R} {project} {C.D}->{C.R} {output}"

    if re.match(r"^\s+\w+\.(cpp|c|cc|cxx)$", line):
        return f"{C.D}[building]{C.R} {line.strip()}"

    if "error" in line.lower():
        return f"{C.RED}x{C.R} {line}"

    if "warning" in line.lower():
        m = re.search(r"\bwarning\s+(C\d+)\b", line)
        if m:
            return f"{C.Y}!{C.R} {m.group(1)}"
        return f"{C.Y}!{C.R} {line}"

    return None


def cmake_generators_text():
    r = run(["cmake", "--help"], capture=True)
    if r.returncode != 0:
        return ""
    return r.stdout


def has_generator(help_text: str, name: str) -> bool:
    return name.lower() in help_text.lower()


def has_ninja_executable() -> bool:
    return shutil.which("ninja") is not None


def path_has_entries(path: Path) -> bool:
    try:
        next(path.iterdir())
        return True
    except StopIteration:
        return False
    except OSError:
        return False


def gitlink_commit_for(path: Path):
    rel = path.relative_to(ROOT).as_posix()
    r = run(["git", "ls-tree", "HEAD", rel], cwd=ROOT, capture=True)
    if r.returncode != 0:
        return None

    line = (r.stdout or "").strip()
    if not line:
        return None

    m = re.match(r"160000 commit ([0-9a-fA-F]{40})\t", line)
    if not m:
        return None
    return m.group(1)


def repo_head_commit(path: Path):
    r = run(["git", "-C", str(path), "rev-parse", "HEAD"], capture=True)
    if r.returncode != 0:
        return None
    return (r.stdout or "").strip() or None


def checkout_repo_commit(path: Path, commit: str, name: str):
    current = repo_head_commit(path)
    if current and current.lower() == commit.lower():
        return

    log(f"-> syncing {name} to {commit[:12]}", C.D)
    r = run(["git", "-C", str(path), "checkout", commit], capture=True)
    if r.returncode != 0:
        details = "\n".join(x for x in [r.stdout, r.stderr] if x)
        raise RuntimeError(f"failed to checkout {name} to {commit}\n{details}")


def ensure_external_dependencies():
    git = shutil.which("git")
    if not git:
        raise RuntimeError("git is required to clone external dependencies automatically")

    external_root = ROOT / "external"
    external_root.mkdir(exist_ok=True)

    for dep in EXTERNAL_DEPS:
        dep_dir = dep["path"]
        pinned_commit = gitlink_commit_for(dep_dir)

        if dep["marker"].exists():
            if pinned_commit and (dep_dir / ".git").exists():
                checkout_repo_commit(dep_dir, pinned_commit, dep["name"])
            continue

        dep_dir.parent.mkdir(parents=True, exist_ok=True)

        if dep_dir.exists() and path_has_entries(dep_dir):
            raise RuntimeError(
                f"{dep['name']} is present but incomplete at {dep_dir}. "
                f"Remove the folder and rerun the build."
            )

        log(f"-> fetching {dep['name']}", C.D)
        clone_cmd = [git, "clone", dep["url"], str(dep_dir)]
        r = run(clone_cmd, capture=True)
        if r.returncode != 0:
            details = "\n".join(x for x in [r.stdout, r.stderr] if x)
            raise RuntimeError(f"failed to clone {dep['name']} from {dep['url']}\n{details}")

        if pinned_commit:
            checkout_repo_commit(dep_dir, pinned_commit, dep["name"])

        if not dep["marker"].exists():
            raise RuntimeError(f"{dep['name']} clone completed but expected files are still missing in {dep_dir}")


def pick_generator(force_ninja=False):
    help_text = cmake_generators_text()
    ninja_available = has_generator(help_text, "Ninja") and has_ninja_executable()

    if force_ninja and ninja_available:
        return ("Ninja", ["-G", "Ninja"], False)

    if force_ninja and not ninja_available:
        log("! --ninja requested but 'ninja' executable is missing, using Visual Studio.", C.Y)

    # Default behavior: prefer Visual Studio.
    if has_generator(help_text, "Visual Studio 18 2026"):
        return ("Visual Studio 18 2026", ["-G", "Visual Studio 18 2026", "-A", "x64"], True)
    if has_generator(help_text, "Visual Studio 17 2022"):
        return ("Visual Studio 17 2022", ["-G", "Visual Studio 17 2022", "-A", "x64"], True)

    if ninja_available:
        return ("Ninja", ["-G", "Ninja"], False)

    return ("(default)", [], False)


def read_cache_generator(build_dir: Path):
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        return None
    try:
        with cache_file.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("CMAKE_GENERATOR:INTERNAL="):
                    return line.split("=", 1)[1].strip()
                if line.startswith("CMAKE_GENERATOR:STRING="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        return None
    return None


def generator_matches(a: str, b: str) -> bool:
    if not a or not b:
        return False
    return a.strip().lower() == b.strip().lower()


def is_multi_config_generator(name: str) -> bool:
    if not name:
        return False
    lowered = name.lower()
    return "visual studio" in lowered or "xcode" in lowered or "multi-config" in lowered


def pick_build_dir(force_fast: bool) -> Path:
    if force_fast:
        return BUILD_FAST
    return BUILD_DEFAULT


def exe_path_for(build_dir: Path, config: str) -> Path:
    p1 = build_dir / config / "Nebula.exe"
    p2 = build_dir / "Nebula.exe"
    return p1 if p1.exists() else p2


def main():
    raw_args = sys.argv[1:]
    args = [a.lower() for a in raw_args]

    force_fast = "--fast" in args
    force_ninja = "--ninja" in args or force_fast
    skip_configure = "--skip-configure" in args or "--no-configure" in args

    if "clean" in args or "--clean" in args or "-c" in args:
        log(f"-> cleaning {BUILD_DEFAULT}", C.D)
        shutil.rmtree(BUILD_DEFAULT, ignore_errors=True)
        log(f"-> cleaning {BUILD_FAST}", C.D)
        shutil.rmtree(BUILD_FAST, ignore_errors=True)
        log("ok", C.G)
        return 0

    config = DEFAULT_CONFIG
    if "debug" in args:
        config = "Debug"
    if "release" in args:
        config = "Release"

    app_version = None
    for a in raw_args:
        if a.startswith("--app-version="):
            app_version = a.split("=", 1)[1].strip()
            break
    if not app_version:
        app_version = os.environ.get("NEBULA_APP_VERSION", "").strip() or None

    jobs = os.environ.get("NEBULA_BUILD_JOBS", "").strip()
    if not jobs:
        jobs = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "").strip()
    if not jobs:
        jobs = str(os.cpu_count() or 8)

    log("nebula cmake builder", C.B)
    log(datetime.now().strftime("%H:%M:%S"), C.D)
    log("", C.R)

    gen_name, gen_args, is_multi_config = pick_generator(force_ninja=force_ninja)
    build_dir = pick_build_dir(force_fast)
    build_dir.mkdir(exist_ok=True)

    active_gen = gen_name
    cache_gen = read_cache_generator(build_dir)
    if cache_gen:
        active_gen = cache_gen
        is_multi_config = is_multi_config_generator(cache_gen)

    if skip_configure and cache_gen and gen_name != "(default)" and not generator_matches(gen_name, cache_gen):
        log(f"-> cache generator differs, using cached one due --skip-configure: {cache_gen}", C.Y)

    if (not skip_configure) and cache_gen and gen_name != "(default)" and not generator_matches(gen_name, cache_gen):
        log(f"-> generator mismatch in {build_dir.name}: cache={cache_gen}, requested={gen_name}", C.Y)
        alt_dir = BUILD_DEFAULT if build_dir == BUILD_FAST else BUILD_FAST
        alt_dir.mkdir(exist_ok=True)
        alt_cache_gen = read_cache_generator(alt_dir)
        if alt_cache_gen and generator_matches(alt_cache_gen, gen_name):
            build_dir = alt_dir
            active_gen = alt_cache_gen
            is_multi_config = is_multi_config_generator(alt_cache_gen)
            log(f"-> switched build dir: {build_dir}", C.Y)
        else:
            cache_file = build_dir / "CMakeCache.txt"
            cmake_files = build_dir / "CMakeFiles"
            if cache_file.exists():
                cache_file.unlink()
            shutil.rmtree(cmake_files, ignore_errors=True)
            active_gen = gen_name
            is_multi_config = is_multi_config_generator(gen_name)
            log(f"-> cleared stale CMake cache in {build_dir}", C.Y)

    log(f"-> build dir: {build_dir}", C.D)
    log(f"-> generator: {active_gen}", C.D)
    log(f"-> parallel jobs: {jobs}", C.D)

    ensure_external_dependencies()

    if skip_configure and (build_dir / "CMakeCache.txt").exists():
        log("-> configuring (skipped)", C.D)
    else:
        log("-> configuring", C.D)
        cmake_configure_cmd = [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(build_dir),
            *gen_args,
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]
        if app_version:
            cmake_configure_cmd.append(f"-DNEBULA_APP_VERSION={app_version}")
            log(f"  app version: {app_version}", C.D)

        r = run(cmake_configure_cmd, capture=True)
        if r.returncode != 0:
            log("x cmake configuration failed", C.RED)
            print(r.stdout)
            print(r.stderr)
            return r.returncode

        for line in (r.stdout or "").splitlines():
            if "Windows SDK" in line or "Using" in line or "libvterm" in line or "ggwave" in line:
                log(f"  {line.strip()}", C.D)

    log(f"\n-> building ({config.lower()})", C.D)
    build_cmd = ["cmake", "--build", str(build_dir), "--parallel", jobs, "--target", "Nebula"]
    if is_multi_config:
        build_cmd += ["--config", config]

    ret, build_lines = run_stream(build_cmd, cwd=build_dir, filter_fn=parse_build_output, collect=True)
    if ret != 0:
        can_retry_serial = False
        try:
            can_retry_serial = int(jobs) > 1
        except ValueError:
            can_retry_serial = False

        if can_retry_serial and has_parallel_lock_error(build_lines):
            log("\n! file lock detected in parallel build, retrying once with jobs=1", C.Y)
            retry_cmd = ["cmake", "--build", str(build_dir), "--parallel", "1", "--target", "Nebula"]
            if is_multi_config:
                retry_cmd += ["--config", config]

            ret_retry, _ = run_stream(retry_cmd, cwd=build_dir, filter_fn=parse_build_output, collect=True)
            if ret_retry != 0:
                log("\nx build failed", C.RED)
                return ret_retry
        else:
            log("\nx build failed", C.RED)
            return ret

    exe = exe_path_for(build_dir, config)
    if exe.exists():
        size_kb = exe.stat().st_size / 1024
        log(f"\nok {exe.name} ({size_kb:.1f} kb)", C.G)
        log(f"  {exe.resolve()}\n", C.D)
        return 0

    log("\n! build completed but Nebula.exe not found", C.Y)
    log(f"  searched: {build_dir / config / 'Nebula.exe'} and {build_dir / 'Nebula.exe'}\n", C.D)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        log("\n! interrupted\n", C.Y)
        raise SystemExit(1)
    except Exception as e:
        log(f"\nx {e}\n", C.RED)
        raise SystemExit(1)
