#pragma once
#include <windows.h>
#include <string>

class Window; // forward declaration

class KeyboardManager
{
public:
    KeyboardManager() = default;

    // Appelé une fois (au WM_CREATE) pour brancher les actions
    void Init(Window* window);

    // WM_KEYDOWN
    bool OnKeyDown(WPARAM wParam);

    // WM_CHAR
    bool OnChar(WPARAM wParam);

    // Start tampon edit mode (menu action)
    void BeginTamponEdit();

private:
    struct Mods
    {
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
    };

private:
    Window* window_ = nullptr;
    bool chordActive_ = false;
    wchar_t chordFirst_ = 0;
    ULONGLONG chordExpiresAt_ = 0;
    bool tamponEditActive_ = false;
    std::wstring tamponEditBuffer_;

private:
    static Mods GetMods();
    static bool IsLetter(WPARAM wParam, wchar_t upper); // 'A'..'Z'

    // 1) Global shortcuts (ne doivent pas être bouffés par focus)
    bool HandleGlobalShortcuts(WPARAM wParam, const Mods& m);
    bool HandleChordShortcut(WPARAM wParam, const Mods& m);
    void StartChord(wchar_t first);
    void ClearChord();
    bool HandleTamponEditKeyDown(WPARAM wParam, const Mods& m);
    bool HandleTamponEditChar(WPARAM wParam);
    void UpdateTamponHint();

    // 2) Routing vers le focus (Explorer/SearchPanel/Terminal/Editor)
    bool RouteKeyDownToFocused(WPARAM wParam);
    bool RouteCharToFocused(WPARAM wParam);

private:
    // Helpers actions
    void OpenFileDialog();
    void OpenProjectDialog();
    void SaveActiveTab();
    void CloseActiveTab();
};
