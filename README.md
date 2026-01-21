# Nebula [![Nebula](assets/badge.svg)](https://github.com/sindresorhus/awesome#readme)

<img src="assets/favicon.ico" align="right" width="96" height="96"/>

> **Nebula is fast **  
> A lightweight, native code editor focused on performance, simplicity, and control.

Nebula is a **native code editor for Windows**, written primarily in **C++ (Win32 + Direct2D/DirectWrite)**.  
It is designed to launch instantly, stay responsive on large projects, and avoid unnecessary abstractions.

Unlike Electron-based editors, Nebula runs **directly on the system**, with minimal memory usage and predictable performance.

---

## Table of Contents

- [Features](#features)
- [Why Nebula?](#why-nebula)
- [Screenshots](#screenshots)
- [Requirements](#requirements)
- [Build Instructions](#build-instructions)
- [Project Structure](#project-structure)
- [External Dependencies](#external-dependencies)
- [Development Status](#development-status)
- [Roadmap](#roadmap)
- [Philosophy](#philosophy)
- [Contributing](#contributing)
- [License](#license)

---

## Features

- ⚡ **Instant startup** (typically < 1 second)
- 🧠 **Native rendering** (Win32 + Direct2D + DirectWrite)
- 📄 Efficient handling of **large files**
- 🧩 Tab system with:
  - Dirty state indicator (●)
  - Smart close behavior
  - MRU (Most Recently Used) logic
- 🖱️ Precise caret, selection, and hit-testing
- 🧭 Explorer with file icons
- 🧵 Asynchronous file loading (non-blocking UI)
- 🎨 Custom theme & rendering pipeline
- ⌨️ Keyboard-first workflow

> Nebula does **not** embed a browser, runtime VM, or web engine.

---

## Why Nebula?

Most modern editors trade performance for convenience.

Nebula makes a different choice:

| Aspect            | Nebula                         |
|-------------------|--------------------------------|
| Rendering         | Native (Direct2D / DWrite)     |
| UI Framework      | Win32 (no Electron, no Qt)     |
| Startup Time      | Instant                        |
| Memory Usage      | Minimal                        |
| Control           | Full ownership of the stack    |

Nebula is built for developers who care about:
- deterministic performance
- understanding their tools
- minimal overhead

---

## Screenshots

> _(Coming soon)_  
You can already see development previews in the commit history.

---

## Requirements

- Windows 10 or later
- C++ compiler (MSVC recommended)
- Python **3.10+**
- Git

---

## Build Instructions

Nebula uses a **Python-based build system**.

### 0. Install dependencies (Windows)

You can use the setup script below to install Python, Git, CMake, GCC (via MSYS2),
install Python requirements, and clone the external dependencies:

```bat
scripts\setup_dependencies.bat
```

### 1. Clone the repository

```bash
git clone https://github.com/Rayan-Walnut/Nebula.git
cd Nebula
````

### 2. Clone external dependencies

Nebula relies on a few native libraries that must be placed in the `external/` directory.

Expected layout:

```text
external/
├─ ggwave/
├─ libvterm/
└─ nanosvg/
```

> These repositories are **not vendored** to keep Nebula lightweight and transparent.

### 3. Build the project

From the root directory:

```bash
python scripts/build.py
```

The script will:

* configure the project
* compile native code
* produce the Nebula executable

---

## Project Structure

```text
Nebula/
├─ src/            # Core C++ source code
│  ├─ orion/       # Editor engine (caret, rendering, geometry)
│  ├─ ui/          # UI components (tabs, panels, popups)
│  └─ core/        # Window, explorer, application logic
├─ assets/         # Icons, fonts, visual resources
├─ external/       # Third-party native libraries
├─ scripts/        # Build & tooling scripts
└─ README.md
```

---

## External Dependencies

Nebula currently uses:

* **libvterm** — terminal emulation
* **ggwave** — audio signal processing
* **nanosvg** — SVG parsing

All dependencies are native and lightweight.

---

## Development Status

⚠️ **Nebula is under active development.**

* APIs may change
* Some features are experimental
* Stability is improving continuously

That said, the editor is already usable for real projects.

---

## Roadmap

Planned features:

* Syntax highlighting optimizations
* Incremental rendering
* Plugin system
* Language services (LSP-like architecture)
* Custom keybindings
* Multi-caret editing

---

## Philosophy

Nebula follows a few core principles:

* **Native first**
* **No hidden layers**
* **Performance over trends**
* **Understandable codebase**
* **Long-term maintainability**

Nebula is not trying to replace VS Code.
It is exploring a different path.

---

## Contributing

Contributions are welcome.

If you want to:

* improve performance
* clean up rendering logic
* work on editor internals

Feel free to open issues or pull requests.

---

## License

This project is currently released under a permissive license.
See the `LICENSE` file for details.
