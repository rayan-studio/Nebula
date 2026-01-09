#!/usr/bin/env python3
"""
analyze_dump.py

Usage:
  python scripts\analyze_dump.py [--dump PATH] [--pdbs PATH] [--symbols PATH]

This script finds a Nebula minidump (logs/nebula_crash_*.dmp) and runs the
WinDbg CLI (`cdb.exe`) to execute a set of diagnostic commands automatically.
It saves the raw WinDbg output to `logs/dump_analysis_YYYYMMDD_HHMMSS.txt` and
prints a short summary (symbol name, exception code, failure bucket) to stdout.

Requirements:
- Debugging Tools for Windows (cdb.exe) must be installed and reachable via PATH
  or located in a typical Windows Kits folder (the script tries common locations).

Example:
  python scripts\analyze_dump.py --dump logs\nebula_crash_20260109_010610.dmp --pdbs build\\Release
"""

import argparse
import glob
import os
import subprocess
import sys
from datetime import datetime


def find_latest_dump(logs_dir='logs'):
    pattern = os.path.join(logs_dir, 'nebula_crash_*.dmp')
    files = glob.glob(pattern)
    if not files:
        return None
    files.sort(key=os.path.getmtime, reverse=True)
    return files[0]


def find_cdb():
    # Try PATH first
    for name in ('cdb.exe', 'cdbw.exe'):
        path = shutil_which(name)
        if path:
            return path

    # Common Win10 SDK paths
    candidates = [
        os.path.join(os.environ.get('ProgramFiles(x86)', 'C:\\Program Files (x86)'), 'Windows Kits', '10', 'Debuggers', 'x64', 'cdb.exe'),
        os.path.join(os.environ.get('ProgramFiles', 'C:\\Program Files'), 'Windows Kits', '10', 'Debuggers', 'x64', 'cdb.exe'),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def shutil_which(cmd):
    # simple emulation of shutil.which to avoid extra imports in some envs
    from shutil import which
    return which(cmd)


def build_windbg_command(cdb_path, dump_path, pdbs=None, sym_server=None):
    # Compose an equivalent cdb command sequence to run inside -c
    cmds = []
    # set symbol server
    if sym_server:
        cmds.append('.symfix {}'.format(sym_server))
    else:
        cmds.append('.symfix')
    # add local PDB folder if provided
    if pdbs:
        cmds.append('.sympath+ {}'.format(pdbs))
    # reload symbols
    cmds.append('.reload /f')
    # verbose analysis
    cmds.append('!analyze -v')
    # switch to exception context
    cmds.append('.ecxr')
    # print stack with parameters + source if available
    cmds.append('kpn')
    # print registers
    cmds.append('r')
    # quit debugger
    cmds.append('q')

    cmd_str = '; '.join(cmds)
    # Build final invocation
    return [cdb_path, '-z', dump_path, '-c', cmd_str]


def run_and_capture(cmd, out_path):
    print('Running:', ' '.join(cmd))
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = proc.stdout
    with open(out_path, 'w', encoding='utf-8', errors='replace') as f:
        f.write(out)
    return out


def summarize_output(out):
    summary = {}
    for line in out.splitlines():
        line = line.strip()
        if line.startswith('EXCEPTION_CODE_STR') or line.startswith('EXCEPTION_CODE:') or 'EXCEPTION_CODE' in line:
            # opportunistic capture
            if 'EXCEPTION' in line and ':' in line:
                key, val = line.split(':', 1)
                summary[key.strip()] = val.strip()
        if line.startswith('SYMBOL_NAME') or line.startswith('MODULE_NAME') or line.startswith('IMAGE_NAME') or line.startswith('FAILURE_BUCKET_ID'):
            if ':' in line:
                k, v = line.split(':', 1)
                summary[k.strip()] = v.strip()
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dump', help='Path to dump file (default: latest nebula_crash_*.dmp in logs/)')
    parser.add_argument('--pdbs', help='Path to local PDBs (e.g., build\\Release)')
    parser.add_argument('--symbols', help='Symbol server URL (defaults to Microsoft public)')
    args = parser.parse_args()

    dump_path = args.dump or find_latest_dump()
    if not dump_path or not os.path.exists(dump_path):
        print('No dump file found. Provide --dump PATH or place nebula_crash_*.dmp in logs/.')
        sys.exit(2)

    cdb_path = find_cdb()
    if not cdb_path:
        print('Could not find cdb.exe. Install Windows Debugging Tools or add cdb.exe to PATH.')
        sys.exit(3)

    ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    out_path = os.path.join('logs', f'dump_analysis_{ts}.txt')

    sym_server = args.symbols or 'https://msdl.microsoft.com/download/symbols'
    cmd = build_windbg_command(cdb_path, dump_path, pdbs=args.pdbs, sym_server=sym_server)
    out = run_and_capture(cmd, out_path)

    print('\nAnalysis saved to:', out_path)
    summary = summarize_output(out)
    if summary:
        print('\nSummary:')
        for k, v in summary.items():
            print(f'- {k}: {v}')
    else:
        print('\nNo obvious summary fields found in output; open the analysis file for details.')


if __name__ == '__main__':
    main()
