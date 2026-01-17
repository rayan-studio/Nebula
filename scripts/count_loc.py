#!/usr/bin/env python3
import os
from collections import defaultdict

# Extensions considérées comme "code"
CODE_EXTS = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".inl", ".ipp",
    ".cs", ".java", ".kt", ".swift",
    ".py", ".js", ".ts", ".tsx", ".jsx",
    ".go", ".rs",
    ".html", ".css", ".scss",
    ".json", ".xml", ".yml", ".yaml",
    ".cmake", ".txt", ".md"
}

# Dossiers à ignorer (tu peux en ajouter)
IGNORE_DIRS = {"build", ".git", ".idea", ".vs", "external", "node_modules"}

def is_ignored_dir(path: str) -> bool:
    parts = set(path.replace("\\", "/").split("/"))
    return any(d in parts for d in IGNORE_DIRS)

def count_lines_in_file(path: str) -> int:
    # Compte toutes les lignes (même vides) de façon robuste
    try:
        with open(path, "rb") as f:
            return f.read().count(b"\n") + 1
    except Exception:
        # Si erreur d'encodage/permission, on ignore
        return 0

def main(root="src"):
    total_lines = 0
    total_files = 0
    by_ext_lines = defaultdict(int)
    by_ext_files = defaultdict(int)

    for dirpath, dirnames, filenames in os.walk(root):
        if is_ignored_dir(dirpath):
            dirnames[:] = []
            continue

        # Empêche de descendre dans les dossiers ignorés
        dirnames[:] = [d for d in dirnames if d not in IGNORE_DIRS]

        for name in filenames:
            path = os.path.join(dirpath, name)
            ext = os.path.splitext(name)[1].lower()

            # Cas spécial : CMakeLists.txt
            if name == "CMakeLists.txt":
                ext = ".cmake"

            if ext not in CODE_EXTS:
                continue

            lines = count_lines_in_file(path)
            total_lines += lines
            total_files += 1
            by_ext_lines[ext] += lines
            by_ext_files[ext] += 1

    print(f"📁 Dossier analysé : {root}")
    print(f"📄 Fichiers comptés : {total_files}")
    print(f"🧮 Lignes totales : {total_lines}")
    print("\nDétail par extension :")
    for ext in sorted(by_ext_lines, key=lambda e: by_ext_lines[e], reverse=True):
        print(f"  {ext:6}  {by_ext_files[ext]:4} fichiers  {by_ext_lines[ext]:8} lignes")

if __name__ == "__main__":
    main("src")
