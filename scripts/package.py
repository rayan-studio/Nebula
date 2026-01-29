#!/usr/bin/env python3
"""
package.py — Génère un installeur Inno Setup et une version portable

Features:
- Crée un installeur Windows avec Inno Setup
- Génère une version portable (zip)
- Détecte automatiquement les dépendances (DLLs runtime, SDL, etc.)
- Logs colorés avec rich (optionnel)

Usage:
    python scripts/package.py [--version 1.0.0] [--app-name "Nebula"]
"""
from __future__ import annotations
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Optional

try:
    from rich.console import Console
    from rich.panel import Panel
    RICH_AVAILABLE = True
except Exception:
    RICH_AVAILABLE = False


class PackageBuilder:
    def __init__(self, args):
        self.args = args
        self.console = Console() if RICH_AVAILABLE else None
        self.project_root = Path(__file__).resolve().parents[1]
        self.build_dir = self.project_root / args.build_dir
        self.dist_dir = self.project_root / args.output_dir
        self.app_name = args.app_name
        self.version = args.version
        
    def log(self, msg: str, style: str = ""):
        """Log avec style si rich disponible"""
        if RICH_AVAILABLE and self.console:
            self.console.print(msg, style=style)
        else:
            print(msg)
    
    def log_panel(self, text: str, style: str = "bold white"):
        """Affiche un panel"""
        if RICH_AVAILABLE and self.console:
            self.console.print(Panel(text, style=style))
        else:
            print(f"== {text} ==")
    
    def find_executable(self) -> Optional[Path]:
        """Trouve l'exécutable compilé"""
        search_paths = [
            self.build_dir / self.args.config / f"{self.app_name}.exe",
            self.build_dir / f"{self.app_name}.exe",
            self.build_dir / "bin" / self.args.config / f"{self.app_name}.exe",
        ]
        
        for path in search_paths:
            if path.exists():
                self.log(f"[green]✓[/green] Exécutable trouvé: {path}")
                return path
        
        self.log(f"[red]✗[/red] Exécutable introuvable. Chemins testés:", style="red")
        for path in search_paths:
            self.log(f"  - {path}")
        return None
    
    def find_system_dlls(self) -> list[Path]:
        """Trouve les DLLs système nécessaires (MSVC runtime, etc.)"""
        dlls = []
        
        # Chemins communs pour les DLLs système
        common_paths = [
            Path(os.environ.get("SystemRoot", "C:\\Windows")) / "System32",
            Path(os.environ.get("ProgramFiles(x86)", "C:\\Program Files (x86)")) / "Microsoft Visual Studio",
        ]
        
        # DLLs runtime communes
        runtime_dlls = [
            "vcruntime140.dll",
            "vcruntime140_1.dll", 
            "msvcp140.dll",
            "concrt140.dll",
        ]
        
        for dll_name in runtime_dlls:
            for base_path in common_paths:
                if not base_path.exists():
                    continue
                for dll_path in base_path.rglob(dll_name):
                    if dll_path.exists() and dll_path not in dlls:
                        dlls.append(dll_path)
                        self.log(f"  Trouvé: {dll_name}")
                        break
        
        return dlls
    
    def collect_dependencies(self, exe_path: Path) -> list[Path]:
        deps = []
        # Ajoute le dossier toolchains et current.json si présents
        toolchains_dir = self.project_root / "external" / "Nebula Studio 2026" / "toolchains"
        if toolchains_dir.exists():
            deps.append(toolchains_dir)
            self.log(f"  Ajouté: toolchains/ (depuis Nebula Studio 2026)")
            current_json = toolchains_dir / "current.json"
            if current_json.exists():
                deps.append(current_json)
                self.log(f"  Ajouté: current.json")
        search_dirs = [
            exe_path.parent,
            self.build_dir / self.args.config,
            self.build_dir / "bin" / self.args.config,
            self.build_dir,
        ]
        
        # Extensions à copier
        extensions = [".dll"]
        if self.args.include_pdb:
            extensions.append(".pdb")
        
        # Collecte des DLLs dans le dossier de build
        for search_dir in search_dirs:
            if not search_dir.exists():
                continue
            for ext in extensions:
                for file in search_dir.glob(f"*{ext}"):
                    if file not in deps:
                        deps.append(file)
                        self.log(f"  Trouvé (build): {file.name}")
        
        # Collecte des DLLs système si demandé
        if self.args.include_runtime:
            self.log("\n[cyan]Recherche des DLLs runtime système...[/cyan]")
            system_dlls = self.find_system_dlls()
            deps.extend(system_dlls)
        
        # Assets/resources si présents
        assets_dir = self.project_root / "assets"
        if assets_dir.exists():
            deps.append(assets_dir)
            self.log(f"  Trouvé (assets): {assets_dir.name}/")

        # Ajoute NebulaDevPrompt.exe depuis Nebula Studio 2026/dist si présent
        nebula_prompt = self.project_root / "external" / "Nebula Studio 2026" / "dist" / "NebulaDevPrompt.exe"
        if nebula_prompt.exists():
            deps.append(nebula_prompt)
            self.log(f"  Ajouté: NebulaDevPrompt.exe (depuis Nebula Studio 2026/dist)")

        # Ajoute NebulaDevShell.exe depuis Nebula Studio 2026/dist si présent
        nebula_shell = self.project_root / "external" / "Nebula Studio 2026" / "dist" / "NebulaDevShell.exe"
        if nebula_shell.exists():
            deps.append(nebula_shell)
            self.log(f"  Ajouté: NebulaDevShell.exe (depuis Nebula Studio 2026/dist)")

        # Ajoute les DLLs externes (SDL, etc.)
        external_dir = self.project_root / "external"
        if external_dir.exists():
            for dll in external_dir.rglob("*.dll"):
                # Prend les DLLs du bon dossier (x64, Release)
                if "x64" in str(dll) or "Release" in str(dll) or dll.parent.name == "bin":
                    if dll not in deps:
                        deps.append(dll)
                        self.log(f"  Trouvé (external): {dll.name}")

        return deps
    
    def create_portable(self, exe_path: Path, deps: list[Path]) -> Optional[Path]:
        """Crée la version portable (ZIP)"""
        self.log_panel("Création de la version portable", "bold cyan")
        
        portable_dir = self.dist_dir / f"{self.app_name}-{self.version}-portable"
        if portable_dir.exists():
            shutil.rmtree(portable_dir)
        portable_dir.mkdir(parents=True, exist_ok=True)
        
        # Copie l'exécutable
        shutil.copy2(exe_path, portable_dir / exe_path.name)
        self.log(f"  Copié: {exe_path.name}")
        
        # Copie les dépendances
        copied_files = set()
        for dep in deps:
            if dep.is_file():
                dest = portable_dir / dep.name
                if dest.name not in copied_files:  # Évite les doublons
                    shutil.copy2(dep, dest)
                    self.log(f"  Copié: {dep.name}")
                    copied_files.add(dest.name)
            elif dep.is_dir():
                dest = portable_dir / dep.name
                shutil.copytree(dep, dest, dirs_exist_ok=True)
                self.log(f"  Copié: {dep.name}/ (dossier)")
        
        # Crée un README
        readme = portable_dir / "README.txt"
        readme.write_text(f"""{self.app_name} v{self.version} - Version Portable

Pour lancer l'application, double-cliquez sur {exe_path.name}

Cette version ne nécessite pas d'installation.
Tous les fichiers nécessaires sont inclus dans ce dossier.

Note: Cette version inclut les bibliothèques requises (DLLs).
Si l'application ne démarre pas, installez Visual C++ Redistributable 2015-2022:
https://aka.ms/vs/17/release/vc_redist.x64.exe
""", encoding="utf-8")
        
        # Crée le ZIP
        zip_path = self.dist_dir / f"{self.app_name}-{self.version}-portable.zip"
        self.log(f"\n[cyan]Création du ZIP...[/cyan]")
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
            for file in portable_dir.rglob("*"):
                if file.is_file():
                    arcname = file.relative_to(portable_dir.parent)
                    zipf.write(file, arcname)
                    
        # Affiche la taille
        size_mb = zip_path.stat().st_size / (1024 * 1024)
        self.log(f"[green]✓[/green] Version portable créée: {zip_path} ({size_mb:.1f} MB)", style="green bold")
        return zip_path
    
    def create_inno_script(self, exe_path: Path, deps: list[Path]) -> Path:
        """Génère le script Inno Setup"""
        script_path = self.dist_dir / "setup_script.iss"
        
        # Liste des fichiers à inclure
        files_section = f'Source: "{exe_path}"; DestDir: "{{app}}"; Flags: ignoreversion\n'

        # Si les DLLs runtime ont été copiées dans la version portable, préfère ces fichiers
        portable_dir = self.dist_dir / f"{self.app_name}-{self.version}-portable"
        copied_files = set()
        for dep in deps:
            if dep.is_file():
                # Priorité au fichier copié dans le portable (évite les problèmes de redirection System32)
                candidate = portable_dir / dep.name
                source_path = candidate if candidate.exists() else dep
                if source_path.exists() and dep.name not in copied_files:
                    files_section += f'Source: "{source_path}"; DestDir: "{{app}}"; Flags: ignoreversion\n'
                    copied_files.add(dep.name)
            elif dep.is_dir():
                # Préfère le dossier copié dans le portable s'il existe
                candidate_dir = portable_dir / dep.name
                if candidate_dir.exists():
                    files_section += f'Source: "{candidate_dir}\\*"; DestDir: "{{app}}\\{dep.name}"; Flags: ignoreversion recursesubdirs\n'
                else:
                    files_section += f'Source: "{dep}\\*"; DestDir: "{{app}}\\{dep.name}"; Flags: ignoreversion recursesubdirs\n'
        
        # Génère un GUID si celui par défaut est utilisé
        app_id = self.args.app_id
        if app_id == "12345678-1234-1234-1234-123456789ABC":
            import uuid
            app_id = str(uuid.uuid4())
            self.log(f"[yellow]Info:[/yellow] GUID généré automatiquement: {app_id}")
        
        # Include app icon (favicon.ico) if present in assets
        icon_path = self.project_root / "assets" / "favicon.ico"
        setup_icon_line = ""
        if icon_path.exists():
            # Add icon to files to install (so installed app folder contains it)
            if icon_path.name not in copied_files:
                files_section += f'Source: "{icon_path}"; DestDir: "{{app}}"; Flags: ignoreversion\n'
                copied_files.add(icon_path.name)
            # Use this icon as the installer icon
            setup_icon_line = f"SetupIconFile={icon_path}\n"

        script_content = f"""; Script généré automatiquement par package.py
#define MyAppName "{self.app_name}"
#define MyAppVersion "{self.version}"
#define MyAppPublisher "{self.args.publisher}"
#define MyAppURL "{self.args.url}"
#define MyAppExeName "{exe_path.name}"

[Setup]
AppId={{{{{app_id}}}}}
AppName={{#MyAppName}}
AppVersion={{#MyAppVersion}}
AppPublisher={{#MyAppPublisher}}
AppPublisherURL={{#MyAppURL}}
AppSupportURL={{#MyAppURL}}
AppUpdatesURL={{#MyAppURL}}
DefaultDirName={{autopf}}\\{{#MyAppName}}
DefaultGroupName={{#MyAppName}}
AllowNoIcons=yes
OutputDir={self.dist_dir}
OutputBaseFilename={self.app_name}-{self.version}-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={{app}}\\{{#MyAppExeName}}
{setup_icon_line}
PrivilegesRequired=lowest

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{{cm:CreateDesktopIcon}}"; GroupDescription: "{{cm:AdditionalIcons}}"

[Files]
{files_section}

[Icons]
Name: "{{group}}\\{{#MyAppName}}"; Filename: "{{app}}\\{{#MyAppExeName}}"; IconFilename: "{{app}}\\assets\\favicon.ico"
Name: "{{group}}\\{{cm:UninstallProgram,{{#MyAppName}}}}"; Filename: "{{uninstallexe}}"
Name: "{{autodesktop}}\\{{#MyAppName}}"; Filename: "{{app}}\\{{#MyAppExeName}}"; Tasks: desktopicon; IconFilename: "{{app}}\\assets\\favicon.ico"

[Registry]
; Context menu (per-user, no admin required)
Root: HKCU; Subkey: "Software\\Classes\\*\\shell\\Open with {{#MyAppName}}"; ValueType: string; ValueName: ""; ValueData: "Ouvrir avec {{#MyAppName}}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\\Classes\\*\\shell\\Open with {{#MyAppName}}"; ValueType: string; ValueName: "Icon"; ValueData: "{{app}}\\assets\\favicon.ico"
Root: HKCU; Subkey: "Software\\Classes\\*\\shell\\Open with {{#MyAppName}}\\command"; ValueType: string; ValueName: ""; ValueData: "\"\"{{app}}\\{{#MyAppExeName}}\"\" \"\"%1\"\""

Root: HKCU; Subkey: "Software\\Classes\\Directory\\shell\\Open with {{#MyAppName}}"; ValueType: string; ValueName: ""; ValueData: "Ouvrir avec {{#MyAppName}}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\\Classes\\Directory\\shell\\Open with {{#MyAppName}}"; ValueType: string; ValueName: "Icon"; ValueData: "{{app}}\\assets\\favicon.ico"
Root: HKCU; Subkey: "Software\\Classes\\Directory\\shell\\Open with {{#MyAppName}}\\command"; ValueType: string; ValueName: ""; ValueData: "\"\"{{app}}\\{{#MyAppExeName}}\"\" \"\"%1\"\""

Root: HKCU; Subkey: "Software\\Classes\\Directory\\Background\\shell\\Open with {{#MyAppName}}"; ValueType: string; ValueName: ""; ValueData: "Ouvrir {{#MyAppName}} ici"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\\Classes\\Directory\\Background\\shell\\Open with {{#MyAppName}}"; ValueType: string; ValueName: "Icon"; ValueData: "{{app}}\\assets\\favicon.ico"
Root: HKCU; Subkey: "Software\\Classes\\Directory\\Background\\shell\\Open with {{#MyAppName}}\\command"; ValueType: string; ValueName: ""; ValueData: "\"\"{{app}}\\{{#MyAppExeName}}\"\" \"\"%V\"\""

[Run]
Filename: "{{app}}\\{{#MyAppExeName}}"; Description: "{{cm:LaunchProgram,{{#StringChange(MyAppName, '&', '&&')}}}}"; Flags: nowait postinstall skipifsilent
"""
        
        script_path.write_text(script_content, encoding="utf-8")
        self.log(f"[green]✓[/green] Script Inno Setup généré: {script_path}")
        return script_path
    
    def compile_installer(self, script_path: Path) -> Optional[Path]:
        """Compile l'installeur avec Inno Setup"""
        self.log_panel("Compilation de l'installeur", "bold magenta")
        
        # Chemins possibles pour ISCC.exe
        iscc_paths = [
            Path(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"),
            Path(r"C:\Program Files\Inno Setup 6\ISCC.exe"),
            Path(r"C:\Program Files (x86)\Inno Setup 5\ISCC.exe"),
            Path(r"C:\Program Files\Inno Setup 5\ISCC.exe"),
        ]
        
        iscc_exe = None
        for path in iscc_paths:
            if path.exists():
                iscc_exe = path
                break
        
        if not iscc_exe:
            self.log("[yellow]⚠[/yellow] Inno Setup non trouvé. Installeur non créé.", style="yellow")
            self.log("  Téléchargez Inno Setup: https://jrsoftware.org/isdl.php")
            return None
        
        self.log(f"  Utilisation de: {iscc_exe}")
        
        try:
            result = subprocess.run(
                [str(iscc_exe), str(script_path)],
                capture_output=True,
                text=True,
                check=True
            )
            
            # Affiche seulement les lignes importantes
            for line in result.stdout.split('\n'):
                if 'Successful' in line or 'Output' in line or 'Compiled' in line:
                    self.log(f"  {line.strip()}")
            
            installer_path = self.dist_dir / f"{self.app_name}-{self.version}-setup.exe"
            if installer_path.exists():
                size_mb = installer_path.stat().st_size / (1024 * 1024)
                self.log(f"[green]✓[/green] Installeur créé: {installer_path} ({size_mb:.1f} MB)", style="green bold")
                return installer_path
        except subprocess.CalledProcessError as e:
            self.log(f"[red]✗[/red] Erreur compilation Inno Setup", style="red")
            self.log("\nDétails de l'erreur:")
            for line in e.stderr.split('\n'):
                if line.strip():
                    self.log(f"  {line}")
        
        return None
    
    def build(self) -> int:
        """Lance le processus complet"""
        self.log_panel(f"Package Builder - {self.app_name} v{self.version}", "bold white on blue")
        
        # Crée le dossier de sortie
        self.dist_dir.mkdir(parents=True, exist_ok=True)
        
        # Trouve l'exécutable
        exe_path = self.find_executable()
        if not exe_path:
            self.log_panel("Erreur: Exécutable introuvable", "bold red")
            return 1
        
        # Collecte les dépendances
        self.log("\n[cyan]Collecte des dépendances...[/cyan]")
        deps = self.collect_dependencies(exe_path)
        self.log(f"\n  Total: {len(deps)} fichier(s)/dossier(s) de dépendances")
        
        if not self.args.include_runtime:
            self.log("\n[yellow]Note:[/yellow] Les DLLs runtime système ne sont pas incluses.")
            self.log("       Utilisez --include-runtime pour les inclure automatiquement.")
        
        # Crée la version portable
        if not self.args.skip_portable:
            portable_zip = self.create_portable(exe_path, deps)
            if not portable_zip:
                return 1
        
        # Crée l'installeur
        if not self.args.skip_installer:
            script_path = self.create_inno_script(exe_path, deps)
            installer_path = self.compile_installer(script_path)
        
        self.log("\n")
        self.log_panel("✓ Packaging terminé avec succès", "bold green")
        self.log(f"\n[cyan]Fichiers générés dans:[/cyan] {self.dist_dir}")
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Package Nebula en version installeur et portable")
    parser.add_argument("--version", default="1.0.0", help="Version de l'application")
    parser.add_argument("--app-name", default="Nebula", help="Nom de l'application")
    parser.add_argument("--build-dir", default="build", help="Dossier de build")
    parser.add_argument("--output-dir", default="dist", help="Dossier de sortie")
    parser.add_argument("--config", default="Release", help="Configuration (Release/Debug)")
    parser.add_argument("--publisher", default="Nebula Company", help="Nom de l'éditeur")
    parser.add_argument("--url", default="https://example.com", help="URL du site")
    parser.add_argument("--app-id", default="12345678-1234-1234-1234-123456789ABC", help="GUID pour Inno Setup")
    parser.add_argument("--skip-installer", action="store_true", help="Ne pas créer l'installeur")
    parser.add_argument("--skip-portable", action="store_true", help="Ne pas créer la version portable")
    parser.add_argument("--include-pdb", action="store_true", help="Inclure les fichiers .pdb (debug)")
    parser.add_argument("--include-runtime", action="store_true", help="Inclure les DLLs runtime système (vcruntime, msvcp)")
    
    args = parser.parse_args(argv)
    
    builder = PackageBuilder(args)
    return builder.build()


if __name__ == "__main__":
    sys.exit(main())
