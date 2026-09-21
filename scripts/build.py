#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import time
import shutil
import subprocess
from pathlib import Path
from datetime import datetime

class C:
    R   = "\033[0m"
    B   = "\033[1m"
    D   = "\033[2m"
    G   = "\033[32m"
    Y   = "\033[33m"
    RED = "\033[31m"
    CY  = "\033[36m"


if sys.platform == "win32":
    os.system("")


def log(msg, color=""):
    print(f"{color}{msg}{C.R}")


ROOT          = Path(__file__).resolve().parent.parent
BUILD_DEFAULT = ROOT / "build"
BUILD_FAST    = ROOT / "build-ninja"
DEFAULT_CONFIG = "Release"
EXTERNAL_DEPS = [
    {
        "name": "libvterm",
        "path": ROOT / "external" / "libvterm",
        "marker": ROOT / "external" / "libvterm" / "include" / "vterm.h",
        "url": "https://github.com/neovim/libvterm.git",
        "type": "git",
    },
    {
        "name": "ggwave",
        "path": ROOT / "external" / "ggwave",
        "marker": ROOT / "external" / "ggwave" / "include" / "ggwave" / "ggwave.h",
        "url": "https://github.com/ggerganov/ggwave.git",
        "type": "git",
    },
    {
        "name": "nanosvg",
        "path": ROOT / "external" / "nanosvg",
        "marker": ROOT / "external" / "nanosvg" / "src" / "nanosvg.h",
        "url": "https://github.com/memononen/nanosvg.git",
        "type": "git",
    },
    {
        "name": "libgit2",
        "path": ROOT / "external" / "libgit2",
        "marker": ROOT / "external" / "libgit2" / "include" / "git2.h",
        "url": "https://github.com/libgit2/libgit2.git",
        "type": "git",
    },
    {
        "name": "clangd",
        "path": ROOT / "external" / "clangd",
        "marker": ROOT / "external" / "clangd" / "bin" / "clangd.exe",
        "url": "https://github.com/clangd/clangd/releases/download/18.1.3/clangd-windows-18.1.3.zip",
        "type": "zip",
        "zip_prefix": "clangd",
    },
    {
        "name": "mingw",
        "path": ROOT / "external" / "mingw",
        "marker": ROOT / "external" / "mingw" / "bin" / "gcc.exe",
        "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260311/llvm-mingw-20260311-ucrt-x86_64.zip",
        "type": "zip",
        "zip_prefix": "llvm-mingw",
    },
]


# ── helpers ───────────────────────────────────────────────────────────────────

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


# ── progress bar ──────────────────────────────────────────────────────────────

BAR_WIDTH = 38

def _fmt_size(n):
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f} MB"
    if n >= 1_000:
        return f"{n / 1_000:.1f} kB"
    return f"{n} B"

def _fmt_speed(bps):
    if bps >= 1_000_000:
        return f"{bps / 1_000_000:.1f} MB/s"
    if bps >= 1_000:
        return f"{bps / 1_000:.0f} kB/s"
    return f"{bps:.0f} B/s"

def _draw_bar(downloaded, total, speed):
    if total:
        frac  = min(downloaded / total, 1.0)
        filled = int(BAR_WIDTH * frac)
        bar   = "━" * filled + " " * (BAR_WIDTH - filled)
        sizes = f"{_fmt_size(downloaded)}/{_fmt_size(total)}"
    else:
        bar   = "━" * BAR_WIDTH
        sizes = _fmt_size(downloaded)
    spd = f"  {_fmt_speed(speed)}" if speed > 0 else ""
    sys.stdout.write(f"\r     {C.G}{bar}{C.R}  {C.D}{sizes}{spd}{C.R}  ")
    sys.stdout.flush()


# ── build output parser ───────────────────────────────────────────────────────

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
        m = re.search(r"(\w[\w\-]*)\.vcxproj -> .*[/\\]([\w.\-]+)$", line)
        if m:
            name = m.group(1)
            return f"  {C.D}{name:<24}{C.R} {C.G}checked{C.R}"

    if re.match(r"^\s+\w+\.(cpp|c|cc|cxx)$", line):
        return None  # suppress individual file lines

    if "error" in line.lower():
        # Trim MSBuild path prefix noise
        clean = re.sub(r"^.*?(\w+\.(cpp|c|h|hpp))", r"\1", line).strip()
        return f"  {C.RED}error{C.R}  {clean}"

    if "warning" in line.lower():
        m = re.search(r"\bwarning\s+(C\d+)\b[:\s]*(.*)", line)
        if m:
            code, msg = m.group(1), m.group(2).strip()
            return f"  {C.Y}warning {code}{C.R}  {C.D}{msg[:80]}{C.R}"
        return None

    return None


# ── git / zip dependency helpers ──────────────────────────────────────────────

def cmake_generators_text():
    r = run(["cmake", "--help"], capture=True)
    if r.returncode != 0:
        return ""
    return r.stdout


def default_cmake_generator(help_text: str) -> str:
    m = re.search(r"^\*\s*(.+?)\s*=", help_text, re.MULTILINE)
    return m.group(1).strip() if m else ""


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
    log(f"  syncing {name} to {commit[:12]}...", C.D)
    r = run(["git", "-C", str(path), "checkout", commit], capture=True)
    if r.returncode != 0:
        details = "\n".join(x for x in [r.stdout, r.stderr] if x)
        raise RuntimeError(f"failed to checkout {name} to {commit}\n{details}")


def download_and_extract_zip(url: str, extract_to: Path, name: str, zip_prefix: str = ""):
    import urllib.request
    import zipfile
    import tempfile

    filename = url.split("/")[-1]

    # HEAD request to get content-length
    total_size = 0
    try:
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req) as resp:
            cl = resp.headers.get("Content-Length")
            if cl:
                total_size = int(cl)
    except Exception:
        pass

    size_str = f" ({_fmt_size(total_size)})" if total_size else ""
    print(f"  {C.D}Downloading {filename}{size_str}{C.R}")

    try:
        with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as tmp:
            tmp_path = tmp.name

        downloaded  = 0
        start_time  = time.monotonic()
        last_draw   = 0.0
        chunk_size  = 65536

        with urllib.request.urlopen(url) as resp, open(tmp_path, "wb") as f:
            while True:
                chunk = resp.read(chunk_size)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                now = time.monotonic()
                elapsed = now - start_time
                speed = downloaded / elapsed if elapsed > 0 else 0
                if now - last_draw >= 0.1:
                    _draw_bar(downloaded, total_size, speed)
                    last_draw = now

        elapsed = time.monotonic() - start_time
        speed   = downloaded / elapsed if elapsed > 0 else 0
        _draw_bar(downloaded, total_size, speed)
        print()  # newline after bar

        extract_to.parent.mkdir(parents=True, exist_ok=True)
        before = set(extract_to.parent.iterdir())
        with zipfile.ZipFile(tmp_path, "r") as zf:
            zf.extractall(extract_to.parent)
        after = set(extract_to.parent.iterdir())

        new_dirs = [p for p in (after - before) if p.is_dir()]
        if zip_prefix:
            new_dirs = [p for p in new_dirs if p.name.startswith(zip_prefix)]
        if new_dirs:
            extracted = new_dirs[0]
            if extract_to.exists():
                shutil.rmtree(extract_to)
            extracted.rename(extract_to)

        Path(tmp_path).unlink(missing_ok=True)
        return True

    except Exception as e:
        print()
        log(f"  error: failed to download {name}: {e}", C.RED)
        return False


def ensure_external_dependencies():
    git = shutil.which("git")
    if not git:
        raise RuntimeError("git not found — required to fetch dependencies")

    external_root = ROOT / "external"
    external_root.mkdir(exist_ok=True)

    for dep in EXTERNAL_DEPS:
        dep_dir  = dep["path"]
        dep_type = dep.get("type", "git")

        if dep["marker"].exists():
            print(f"Collecting {dep['name']:<20} {C.D}already satisfied{C.R}")
            if dep_type == "git":
                pinned_commit = gitlink_commit_for(dep_dir)
                if pinned_commit and (dep_dir / ".git").exists():
                    checkout_repo_commit(dep_dir, pinned_commit, dep["name"])
            continue

        print(f"Collecting {dep['name']}")

        dep_dir.parent.mkdir(parents=True, exist_ok=True)

        if dep_dir.exists() and path_has_entries(dep_dir):
            raise RuntimeError(
                f"{dep['name']} is present but incomplete at {dep_dir}. "
                f"Remove the folder and rerun the build."
            )

        if dep_type == "git":
            print(f"  {C.D}Cloning {dep['url']}{C.R}")
            r = run([git, "clone", dep["url"], str(dep_dir)], capture=True)
            if r.returncode != 0:
                details = "\n".join(x for x in [r.stdout, r.stderr] if x)
                raise RuntimeError(f"failed to clone {dep['name']}\n{details}")
            pinned_commit = gitlink_commit_for(dep_dir)
            if pinned_commit:
                checkout_repo_commit(dep_dir, pinned_commit, dep["name"])

        elif dep_type == "zip":
            success = download_and_extract_zip(
                dep["url"], dep_dir, dep["name"], dep.get("zip_prefix", "")
            )
            if not success:
                raise RuntimeError(f"failed to download {dep['name']}")

        if not dep["marker"].exists():
            raise RuntimeError(
                f"{dep['name']}: installed but expected marker missing in {dep_dir}"
            )


# ── source-file snapshot (detect new/removed .cpp files without --reconfigure) ─

def _src_snapshot_path(build_dir: Path) -> Path:
    return build_dir / ".nebula_src_snapshot"

def _current_src_files() -> set:
    return {p.as_posix() for p in (ROOT / "src").rglob("*.cpp")}

def _saved_src_files(build_dir: Path) -> set:
    snap = _src_snapshot_path(build_dir)
    if not snap.exists():
        return set()
    return set(snap.read_text(encoding="utf-8").splitlines())

def _save_src_snapshot(build_dir: Path):
    _src_snapshot_path(build_dir).write_text(
        "\n".join(sorted(_current_src_files())), encoding="utf-8"
    )

def src_files_changed(build_dir: Path) -> bool:
    """Return True if .cpp files were added or removed since last configure."""
    if not (build_dir / "CMakeCache.txt").exists():
        return True
    return _current_src_files() != _saved_src_files(build_dir)


# ── cmake helpers ─────────────────────────────────────────────────────────────

def pick_generator(force_ninja=False):
    help_text = cmake_generators_text()
    ninja_available = has_generator(help_text, "Ninja") and has_ninja_executable()

    if force_ninja and ninja_available:
        return ("Ninja", ["-G", "Ninja"], False)
    if force_ninja and not ninja_available:
        log("  warning: --ninja requested but ninja not found, using Visual Studio", C.Y)

    default_gen = default_cmake_generator(help_text)
    if default_gen and "visual studio" in default_gen.lower():
        return (default_gen, ["-G", default_gen, "-A", "x64"], True)
    if ninja_available:
        return ("Ninja", ["-G", "Ninja"], False)
    if default_gen:
        return (default_gen, ["-G", default_gen], is_multi_config_generator(default_gen))
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
    return BUILD_FAST if force_fast else BUILD_DEFAULT


def exe_path_for(build_dir: Path, config: str) -> Path:
    p1 = build_dir / config / "Nebula.exe"
    p2 = build_dir / "Nebula.exe"
    return p1 if p1.exists() else p2


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    raw_args = sys.argv[1:]
    args     = [a.lower() for a in raw_args]

    force_fast        = "--fast" in args
    force_ninja       = "--ninja" in args or force_fast
    force_reconfigure = "--reconfigure" in args or "--configure" in args

    if "clean" in args or "--clean" in args or "-c" in args:
        CLEAN_DIRS = [
            BUILD_DEFAULT,
            BUILD_FAST,
            ROOT / "cmake-build-debug",
            ROOT / "cmake-build-release",
            ROOT / "dist",
            ROOT / ".idea",
            ROOT / ".vs",
            ROOT / "logs",
        ]
        for d in CLEAN_DIRS:
            if d.exists():
                log(f"  Removing {d.name}...", C.D)
                shutil.rmtree(d, ignore_errors=True)
        log("Done.", C.G)
        return 0

    config = DEFAULT_CONFIG
    if "debug"   in args: config = "Debug"
    if "release" in args: config = "Release"

    app_version = None
    for a in raw_args:
        if a.startswith("--app-version="):
            app_version = a.split("=", 1)[1].strip()
            break
    if not app_version:
        app_version = os.environ.get("NEBULA_APP_VERSION", "").strip() or None

    jobs = (
        os.environ.get("NEBULA_BUILD_JOBS", "").strip()
        or os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "").strip()
        or str(os.cpu_count() or 8)
    )

    # ── header ────────────────────────────────────────────────────────────────
    print(f"{C.B}Nebula{C.R}  {C.D}{datetime.now().strftime('%H:%M:%S')}  "
          f"{config.lower()}{C.R}")
    print()

    gen_name, gen_args, is_multi_config = pick_generator(force_ninja=force_ninja)
    build_dir = pick_build_dir(force_fast)
    build_dir.mkdir(exist_ok=True)

    active_gen   = gen_name
    cache_gen    = read_cache_generator(build_dir)
    if cache_gen:
        active_gen   = cache_gen
        is_multi_config = is_multi_config_generator(cache_gen)

    cache_exists   = (build_dir / "CMakeCache.txt").exists()
    new_sources    = src_files_changed(build_dir)
    skip_configure = not force_reconfigure and not new_sources and cache_exists

    if skip_configure and cache_gen and gen_name != "(default)" and not generator_matches(gen_name, cache_gen):
        log(f"  note: using cached generator ({cache_gen})", C.D)

    if (not skip_configure) and cache_gen and gen_name != "(default)" and not generator_matches(gen_name, cache_gen):
        alt_dir = BUILD_DEFAULT if build_dir == BUILD_FAST else BUILD_FAST
        alt_dir.mkdir(exist_ok=True)
        alt_cache_gen = read_cache_generator(alt_dir)
        if alt_cache_gen and generator_matches(alt_cache_gen, gen_name):
            build_dir    = alt_dir
            active_gen   = alt_cache_gen
            is_multi_config = is_multi_config_generator(alt_cache_gen)
            log(f"  switched build dir to {build_dir.name}", C.D)
        else:
            cache_file  = build_dir / "CMakeCache.txt"
            cmake_files = build_dir / "CMakeFiles"
            if cache_file.exists():
                cache_file.unlink()
            shutil.rmtree(cmake_files, ignore_errors=True)
            active_gen   = gen_name
            is_multi_config = is_multi_config_generator(gen_name)
            log(f"  cleared stale cache in {build_dir.name}", C.D)

    # ── dependencies ──────────────────────────────────────────────────────────
    ensure_external_dependencies()
    print()

    # ── configure ─────────────────────────────────────────────────────────────
    if skip_configure:
        log(f"Configuring...  {C.D}skipped  (use --reconfigure to force){C.R}", "")
    else:
        sys.stdout.write(f"Configuring...  ")
        sys.stdout.flush()
        cmake_configure_cmd = [
            "cmake", "-S", str(ROOT), "-B", str(build_dir),
            *gen_args,
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]
        if app_version:
            cmake_configure_cmd.append(f"-DNEBULA_APP_VERSION={app_version}")

        r = run(cmake_configure_cmd, capture=True)
        if r.returncode != 0:
            print()
            log("error: cmake configuration failed", C.RED)
            output_lines = []
            if r.stdout:
                output_lines.extend(r.stdout.splitlines())
            if r.stderr:
                output_lines.extend(r.stderr.splitlines())
            for ln in [x for x in output_lines if x.strip()][-20:]:
                log(f"  {ln}", C.D)
            return r.returncode

        _save_src_snapshot(build_dir)
        print(f"{C.G}done{C.R}")

    print()

    # ── build ─────────────────────────────────────────────────────────────────
    log("Building Nebula", C.B)
    build_cmd = ["cmake", "--build", str(build_dir), "--parallel", jobs, "--target", "Nebula"]
    if is_multi_config:
        build_cmd += ["--config", config]

    ret, build_lines = run_stream(build_cmd, cwd=build_dir, filter_fn=parse_build_output, collect=True)

    if ret != 0:
        can_retry = False
        try:
            can_retry = int(jobs) > 1
        except ValueError:
            pass

        if can_retry and has_parallel_lock_error(build_lines):
            log("\n  file lock in parallel build — retrying with jobs=1", C.Y)
            retry_cmd = ["cmake", "--build", str(build_dir), "--parallel", "1", "--target", "Nebula"]
            if is_multi_config:
                retry_cmd += ["--config", config]
            ret_retry, _ = run_stream(retry_cmd, cwd=build_dir, filter_fn=parse_build_output, collect=True)
            if ret_retry != 0:
                print()
                log("error: build failed", C.RED)
                return ret_retry
        else:
            print()
            log("error: build failed", C.RED)
            return ret

    # ── success ───────────────────────────────────────────────────────────────
    exe = exe_path_for(build_dir, config)
    if exe.exists():
        size_kb = exe.stat().st_size / 1024
        print()
        log(f"Successfully built {exe.name} ({size_kb:.1f} kB)", C.G)
        log(f"  {exe.resolve()}", C.D)
        print()
        return 0

    log("\nwarning: build completed but Nebula.exe not found", C.Y)
    log(f"  searched: {build_dir / config / 'Nebula.exe'}", C.D)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print()
        log("\nInterrupted.", C.Y)
        raise SystemExit(1)
    except Exception as e:
        print()
        log(f"\nerror: {e}", C.RED)
        raise SystemExit(1)
