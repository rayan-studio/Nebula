#pragma once
#include <windows.h>
#include <string>

// Kind of dialog — affects the icon/accent drawn in the title bar.
enum class DialogKind
{
    Info,
    Warning,
    Error,
};

// Shows a modal themed dialog that matches the CloneDialog visual style.
// Blocks until the user closes the dialog.
void ShowDialog(HWND parent,
                const std::wstring &title,
                const std::wstring &message,
                DialogKind         kind = DialogKind::Info);

// Shows a modal Yes/No confirmation dialog. Returns true if the user clicked Yes.
bool ShowConfirmDialog(HWND parent,
                       const std::wstring &title,
                       const std::wstring &message,
                       DialogKind          kind = DialogKind::Warning);
