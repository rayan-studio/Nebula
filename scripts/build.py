import subprocess
import sys
import shutil
import re
from pathlib import Path
from datetime import datetime

# ═══════════════════════════════════════════════════════════════════════════
# Colors
# ═══════════════════════════════════════════════════════════════════════════
class C:
    R = '\033[0m'
    B = '\033[1m'
    D = '\033[2m'
    G = '\033[32m'
    Y = '\033[33m'
    RED = '\033[31m'
    C = '\033[36m'

if sys.platform == 'win32':
    import os
    os.system('')

# ═══════════════════════════════════════════════════════════════════════════
# Config
# ═══════════════════════════════════════════════════════════════════════════
ROOT = Path(__file__).parent.parent
BUILD = ROOT / "build"
EXE = BUILD / "Release" / "Nebula.exe"

# ═══════════════════════════════════════════════════════════════════════════
# Utils
# ═══════════════════════════════════════════════════════════════════════════
def log(msg, color=''):
    print(f"{color}{msg}{C.R}")

def run_stream(cmd, filter_fn=None, cwd=None):
    """Run command and stream output with optional filtering"""
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=cwd
    )
    
    for line in proc.stdout:
        line = line.rstrip()
        if filter_fn:
            filtered = filter_fn(line)
            if filtered:
                print(filtered)
        else:
            print(line)
    
    proc.wait()
    return proc.returncode

def parse_msbuild(line):
    """Filter and format MSBuild output"""
    # Skip noise
    skip = [
        'Version MSBuild',
        'Checking File Globs',
        'Checking Build System',
        'Building Custom Rule',
        'Génération de code',
        'compiler le fichier source',
        'with [',
        'warnings',
    ]
    if any(s in line for s in skip):
        return None
    
    # Extract building files
    if '.vcxproj ->' in line:
        match = re.search(r'(\w+)\.vcxproj -> .*[/\\](\w+\.\w+)$', line)
        if match:
            project, output = match.groups()
            return f"{C.D}→{C.R} {project} {C.D}→{C.R} {output}"
    
    # Extract compilation progress
    if re.match(r'^\s+\w+\.(cpp|c)$', line):
        fname = line.strip()
        return f"{C.D}[building]{C.R} {fname}"
    
    # Keep important messages
    if 'error' in line.lower():
        return f"{C.RED}✗{C.R} {line}"
    
    if 'warning C4' in line:
        # Simplify warnings
        match = re.search(r'warning (C\d+):', line)
        if match:
            return f"{C.Y}⚠{C.R} {match.group(1)}"
    
    return None

# ═══════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════
def main():
    log(f"{C.B}nebula{C.R} {C.D}cmake builder{C.R}")
    log(f"{C.D}{datetime.now().strftime('%H:%M:%S')}{C.R}\n")
    
    BUILD.mkdir(exist_ok=True)
    
    # Handle clean
    if len(sys.argv) > 1 and sys.argv[1] in ("clean", "--clean", "-c"):
        log(f"{C.D}→ cleaning build/{C.R}")
        shutil.rmtree(BUILD, ignore_errors=True)
        BUILD.mkdir(exist_ok=True)
        log(f"{C.G}✓ done{C.R}\n")
        return
    
    # Configure
    log(f"{C.D}→ configuring{C.R}")
    r = subprocess.run(
        [
            "cmake",
            str(ROOT),
            "-G",
            "Visual Studio 17 2022",
            "-A",
            "x64",
        ],
        cwd=BUILD,
        capture_output=True,
        text=True
    )
    
    if r.returncode != 0:
        log(f"{C.RED}✗ cmake configuration failed{C.R}\n")
        print(r.stderr)
        sys.exit(1)
    
    # Show brief config summary
    for line in r.stdout.split('\n'):
        if 'Windows SDK' in line or 'Using' in line:
            log(f"{C.D}  {line.strip()}{C.R}")

    # Build
    log(f"\n{C.D}→ building (release){C.R}")
    
    ret = run_stream(
        ["cmake", "--build", ".", "--config", "Release"],
        cwd=BUILD,
        filter_fn=parse_msbuild
    )
    
    if ret != 0:
        log(f"\n{C.RED}✗ build failed{C.R}\n")
        sys.exit(ret)
    
    # Success
    if EXE.exists():
        size = EXE.stat().st_size / 1024
        log(f"\n{C.G}✓{C.R} {EXE.name} {C.D}({size:.1f}kb){C.R}")
        log(f"{C.D}  {EXE.absolute()}{C.R}\n")
    else:
        log(f"\n{C.Y}⚠ build completed but executable not found{C.R}\n")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log(f"\n{C.Y}⚠ interrupted{C.R}\n")
        sys.exit(1)
    except Exception as e:
        log(f"\n{C.RED}✗ {e}{C.R}\n")
        sys.exit(1)
