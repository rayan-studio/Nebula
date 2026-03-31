#include "KeyboardManager.h"

#include "core/window/Window.h"
#include "utils/logger/Logger.h"

#include "core/explorer/Explorer.h"
#include "ui/panels/PanelManager.h"
#include "ui/panels/git/GitPanel.h"
#include "ui/panels/MarketplacePanel.h"
#include "ui/panels/search/SearchPanel.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "ui/components/menu/DropdownMenu.h"
#include "ui/components/input/InputTypeFixed.h"
#include "ui/components/footer/Footer.h"
#include "lsp/LspManager.h"

#include <commdlg.h>
#include <shobjidl.h>
#include <vector>
#include <map>
#include <cwctype>
#include <cmath>

static bool IsCppLikePath(const std::wstring &path)
{
    if (path.empty())
        return false;
    size_t pos = path.find_last_of(L'.');
    if (pos == std::wstring::npos)
        return false;
    std::wstring ext = path.substr(pos + 1);
    for (auto &c : ext) c = (wchar_t)towlower(c);
    return ext == L"c" || ext == L"cpp" || ext == L"cc" || ext == L"cxx" ||
           ext == L"h" || ext == L"hpp" || ext == L"hh" || ext == L"hxx";
}

static constexpr int kEditorSelectionQuickMenuBaseId = 7050;

static bool IsCtrlSemicolonShortcut(WPARAM wParam, bool ctrl, bool alt, bool shift)
{
    if (!ctrl || alt)
        return false;

    // US layout (;: key)
    if (wParam == VK_OEM_1)
        return true;

    // Common FR/AZERTY mapping (';' is often Shift + comma key)
    if ((wParam == VK_OEM_COMMA || wParam == VK_OEM_2) && shift)
        return true;

    // Fallback: keyboard-layout aware check.
    BYTE keyState[256] = {};
    if (shift)
        keyState[VK_SHIFT] = 0x80;
    HKL layout = GetKeyboardLayout(0);
    UINT scanCode = MapVirtualKeyExW((UINT)wParam, MAPVK_VK_TO_VSC, layout);
    wchar_t out[4] = {};
    int rc = ToUnicodeEx((UINT)wParam, scanCode, keyState, out, 4, 0, layout);
    return (rc == 1 && out[0] == L';');
}

static int FindNextEnabledMenuIndex(const MenuDropdown &dd, int current, int direction)
{
    const int count = (int)dd.items.size();
    if (count <= 0)
        return -1;
    const int dir = (direction >= 0) ? 1 : -1;

    int start = current;
    if (start < 0 || start >= count)
        start = (dir > 0) ? -1 : count;

    for (int step = 0; step < count; ++step)
    {
        int idx = start + dir * (step + 1);
        while (idx < 0)
            idx += count;
        while (idx >= count)
            idx -= count;

        bool enabled = true;
        if (!dd.separators.empty() && idx < (int)dd.separators.size() && dd.separators[(size_t)idx])
            enabled = false;
        if (!dd.enabled.empty() && idx < (int)dd.enabled.size() && !dd.enabled[(size_t)idx])
            enabled = false;
        if (enabled)
            return idx;
    }
    return -1;
}

static bool HandleOpenDropdownByKeyboard(HWND hwnd, WPARAM wParam)
{
    if (!IsMenuDropdownVisible())
        return false;

    MenuDropdown &active = GetActiveDropdown();
    MenuDropdown *target = IsSubmenuDropdownVisible() ? &GetSubmenuDropdown() : &active;

    if (wParam == VK_ESCAPE)
    {
        HideSubmenuDropdown(hwnd);
        HideMenuDropdown(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    if (wParam == VK_LEFT && IsSubmenuDropdownVisible())
    {
        HideSubmenuDropdown(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    if (wParam == VK_DOWN || wParam == VK_UP || wParam == VK_TAB)
    {
        int direction = (wParam == VK_UP) ? -1 : 1;
        int next = FindNextEnabledMenuIndex(*target, target->hoveredItem, direction);
        if (next >= 0)
        {
            if (target == &active)
                SetDropdownHoveredItem(next);
            else
                SetSubmenuHoveredItem(next);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }

    if (wParam == VK_RIGHT && !IsSubmenuDropdownVisible())
    {
        int idx = active.hoveredItem;
        if (idx < 0)
            idx = FindNextEnabledMenuIndex(active, -1, 1);
        bool hasSub = (idx >= 0 && !active.hasSubmenu.empty() && idx < (int)active.hasSubmenu.size() && active.hasSubmenu[(size_t)idx]);
        if (hasSub)
        {
            const float itemHeight = (active.rect.bottom - active.rect.top) / (active.items.empty() ? 1.0f : (float)active.items.size());
            const int x = (int)std::lround((active.rect.left + active.rect.right) * 0.5f);
            const int y = (int)std::lround(active.rect.top + ((float)idx + 0.5f) * itemHeight);
            SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        }
        return true;
    }

    if (wParam == VK_RETURN || wParam == VK_SPACE)
    {
        int idx = target->hoveredItem;
        if (idx < 0)
            idx = FindNextEnabledMenuIndex(*target, -1, 1);
        if (idx < 0)
            return true;

        const float itemHeight = (target->rect.bottom - target->rect.top) / (target->items.empty() ? 1.0f : (float)target->items.size());
        const int x = (int)std::lround((target->rect.left + target->rect.right) * 0.5f);
        const int y = (int)std::lround(target->rect.top + ((float)idx + 0.5f) * itemHeight);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        return true;
    }

    return false;
}

KeyboardManager::Mods KeyboardManager::GetMods()
{
    Mods m;
    m.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    m.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    m.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

bool KeyboardManager::IsLetter(WPARAM wParam, wchar_t upper)
{
    return (wParam == (WPARAM)upper);
}

void KeyboardManager::Init(Window *window)
{
    window_ = window;
}

bool KeyboardManager::OnKeyDown(WPARAM wParam)
{
    if (!window_)
        return false;

    if (HandleOpenDropdownByKeyboard(window_->GetHwnd(), wParam))
        return true;

    const Mods m = GetMods();

    if (HandleTamponEditKeyDown(wParam, m))
        return true;

    // Chord shortcuts (ex: Ctrl+K, Ctrl+D)
    if (HandleChordShortcut(wParam, m))
        return true;

    // 1) Global shortcuts FIRST
    if (HandleGlobalShortcuts(wParam, m))
        return true;

    // 2) Sinon route vers focus/mode actif
    return RouteKeyDownToFocused(wParam);
}

bool KeyboardManager::OnChar(WPARAM wParam)
{
    if (!window_)
        return false;

    if (IsMenuDropdownVisible())
        return true;

    // Prevent Ctrl-based shortcuts (e.g. Ctrl+;) from injecting printable chars
    // into focused controls via WM_CHAR.
    const Mods m = GetMods();
    if (m.ctrl && !m.alt)
        return true;

    // WM_CHAR -> route vers l''input actif
    if (HandleTamponEditChar(wParam))
        return true;
    return RouteCharToFocused(wParam);
}

void KeyboardManager::BeginTamponEdit()
{
    if (!window_)
        return;
    tamponEditActive_ = true;
    tamponEditBuffer_ = GetTamponText();
    UpdateTamponHint();
}

void KeyboardManager::UpdateTamponHint()
{
    if (!window_)
        return;

    std::wstring preview = tamponEditBuffer_;
    for (auto &ch : preview)
    {
        if (ch == L'\n' || ch == L'\r')
            ch = L' ';
    }
    if (preview.empty())
        preview = L"(vide)";
    if (preview.size() > 120)
    {
        preview = preview.substr(0, 120);
        preview += L"...";
    }
    Footer_SetHint(window_->GetHwnd(), L"Tampon: " + preview, 0);
}

bool KeyboardManager::HandleTamponEditKeyDown(WPARAM wParam, const Mods &m)
{
    if (!tamponEditActive_)
        return false;

    if (wParam == VK_ESCAPE)
    {
        tamponEditActive_ = false;
        Footer_ClearHint(window_->GetHwnd());
        return true;
    }

    if (wParam == VK_RETURN)
    {
        if (m.shift)
        {
            tamponEditBuffer_.push_back(L'\n');
            UpdateTamponHint();
            return true;
        }

        if (tamponEditBuffer_.empty())
            ClearTamponText();
        else
            SetTamponText(tamponEditBuffer_);

        tamponEditActive_ = false;
        Footer_ClearHint(window_->GetHwnd());
        Footer_SetHint(window_->GetHwnd(), L"Tampon mis a jour", 1500);
        return true;
    }

    if (wParam == VK_BACK)
    {
        if (!tamponEditBuffer_.empty())
            tamponEditBuffer_.pop_back();
        UpdateTamponHint();
        return true;
    }

    if (m.ctrl && !m.alt && (wParam == 'V'))
    {
        if (OpenClipboard(NULL))
        {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData)
            {
                wchar_t *clip = static_cast<wchar_t *>(GlobalLock(hData));
                if (clip)
                {
                    tamponEditBuffer_ += clip;
                    GlobalUnlock(hData);
                }
            }
            CloseClipboard();
        }
        UpdateTamponHint();
        return true;
    }

    return true;
}

bool KeyboardManager::HandleTamponEditChar(WPARAM wParam)
{
    if (!tamponEditActive_)
        return false;

    wchar_t ch = (wchar_t)wParam;
    if (ch >= 32)
    {
        tamponEditBuffer_.push_back(ch);
        UpdateTamponHint();
    }
    return true;
}

void KeyboardManager::StartChord(wchar_t first)
{
    chordActive_ = true;
    chordFirst_ = first;
    chordExpiresAt_ = GetTickCount64() + 2000;
}

void KeyboardManager::ClearChord()
{
    chordActive_ = false;
    chordFirst_ = 0;
    chordExpiresAt_ = 0;
}

bool KeyboardManager::HandleChordShortcut(WPARAM wParam, const Mods &m)
{
    if (!chordActive_)
        return false;

    ULONGLONG now = GetTickCount64();
    if (chordExpiresAt_ != 0 && now > chordExpiresAt_)
    {
        ClearChord();
        return false;
    }

    // Chord expects Ctrl + second key
    if (!m.ctrl || m.alt)
    {
        Footer_SetHint(window_->GetHwnd(), L"Raccourci annule", 1200);
        ClearChord();
        return true;
    }

    if (chordFirst_ == L'K' && !m.shift && IsLetter(wParam, 'D'))
    {
        Orion::Editor *editor = window_->GetEditor();
        if (editor)
        {
            std::wstring path = editor->GetFilePath();
            if (IsCppLikePath(path))
            {
                editor->FormatDocument();
                InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
                GetPanelManager().UpdateLayout(window_->GetHwnd());
                Footer_SetHint(window_->GetHwnd(), L"Format C++ applique", 1500);
            }
            else
            {
                Footer_SetHint(window_->GetHwnd(), L"Format dispo uniquement pour C/C++", 2000);
            }
        }
        ClearChord();
        return true;
    }

    Footer_SetHint(window_->GetHwnd(), L"Raccourci annule", 1200);
    ClearChord();
    return true;
}

// =====================================================
// Actions (Open/Save/Close)
// =====================================================

void KeyboardManager::OpenFileDialog()
{
    wchar_t fileName[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_->GetHwnd(); // il faut exposer GetHwnd() OU utiliser window_->hwnd_ si public
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All Files\0*.*\0Text Files\0*.txt\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn))
    {
        window_->OpenFileInNewTab(std::wstring(fileName), -1);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
    }
}

void KeyboardManager::OpenProjectDialog()
{
    IFileOpenDialog *pFileOpen = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
    if (SUCCEEDED(hr) && pFileOpen)
    {
        DWORD options = 0;
        if (SUCCEEDED(pFileOpen->GetOptions(&options)))
            pFileOpen->SetOptions(options | FOS_PICKFOLDERS);

        if (SUCCEEDED(pFileOpen->Show(window_->GetHwnd())))
        {
            IShellItem *pItem = nullptr;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem)) && pItem)
            {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
                {
                    std::wstring selectedFolder = pszPath;
                    CoTaskMemFree(pszPath);

                    GetExplorerManager().Initialize(selectedFolder);
                    GetExplorerManager().SetVisible(true);

                    InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
}

void KeyboardManager::CloseActiveTab()
{
    int active = window_->GetTabBar()->GetActiveTabIndex();
    if (active < 0)
        return;

    window_->GetTabBar()->CloseTab(active);

    // delete editor + reindex (mÃªme logique que ton code)
    window_->CloseEditorForTabIndex(active);

    InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
}

void KeyboardManager::SaveActiveTab()
{
    int active = window_->GetTabBar()->GetActiveTabIndex();
    if (active < 0)
        return;

    Orion::Editor *editor = window_->GetEditorForTab(active);
    if (!editor)
        return;

    auto requestDiagnosticsForActive = [this, active](Orion::Editor *ed, const std::wstring &path)
    {
        if (!ed || path.empty() || path.rfind(L"__untitled__", 0) == 0)
            return;
        auto lines = ed->GetLinesSnapshot();
        Lsp::LspManager::Instance().UpdateFile(path, lines);
        Lsp::LspManager::Instance().RequestDiagnosticsAsync(path, lines, window_->GetHwnd(), active, true);
    };

    std::wstring currentPath = editor->GetFilePath();
    const bool isUntitled = currentPath.empty() || currentPath.rfind(L"__untitled__", 0) == 0;

    if (isUntitled)
    {
        wchar_t fileName[MAX_PATH] = {0};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = window_->GetHwnd();
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"All Files\0*.*\0Text Files\0*.txt\0\0";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"txt";

        if (GetSaveFileNameW(&ofn))
        {
            std::wstring chosen = fileName;
            if (editor->SaveToFile(chosen))
            {
                size_t lastSlash = chosen.find_last_of(L"\\/");
                std::wstring display = (lastSlash != std::wstring::npos) ? chosen.substr(lastSlash + 1) : chosen;

                window_->GetTabBar()->UpdateTabPath(active, chosen, display);

                // âœ… IMPORTANT: enlever le rond
                window_->GetTabBar()->SetTabDirty(active, false);

                requestDiagnosticsForActive(editor, chosen);

                InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            }
        }
    }
    else
    {
        if (editor->SaveToFile(currentPath))
        {
            // âœ… IMPORTANT: enlever le rond
            window_->GetTabBar()->SetTabDirty(active, false);

            requestDiagnosticsForActive(editor, currentPath);

            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        }
    }
}

// =====================================================
// Global shortcuts
// =====================================================

bool KeyboardManager::HandleGlobalShortcuts(WPARAM wParam, const Mods &m)
{
    if (IsCtrlSemicolonShortcut(wParam, m.ctrl, m.alt, m.shift))
    {
        Orion::Editor *editor = window_->GetEditor();
        if (editor)
        {
            std::wstring selection = editor->GetSelectionText();
            if (!selection.empty())
            {
                POINT pt = {};
                GetCursorPos(&pt);
                ScreenToClient(window_->GetHwnd(), &pt);
                std::vector<std::wstring> items = {
                    L"Deplacer vers fonction"};
                ShowContextMenuDropdown(window_->GetHwnd(), items, D2D1::Point2F((float)pt.x + 8.0f, (float)pt.y + 8.0f),
                                        kEditorSelectionQuickMenuBaseId);
                InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
                return true;
            }
        }
    }

    if (!m.ctrl && !m.shift && !m.alt && wParam == VK_F12)
    {
        int active = window_->GetTabBar()->GetActiveTabIndex();
        Orion::Editor *editor = window_->GetEditorForTab(active);
        if (editor)
        {
            auto loc = editor->TryGoToDefinitionAtCaret();
            if (loc.has_value())
            {
                window_->OpenFileInNewTab(loc->filePath, loc->line, loc->column);
                return true;
            }
        }
        return true;
    }

    // Toggle terminal: Ctrl+`
    if (m.ctrl && !m.shift && !m.alt && (wParam == VK_OEM_3))
    {
        TerminalPanel &terminal = GetTerminalPanel();

        // Toggle visibility
        terminal.ToggleVisible();

        // Ensure layout is updated immediately
        GetPanelManager().UpdateLayout(window_->GetHwnd());

        if (terminal.IsVisible())
        {
            terminal.SetFocused(true);

            // If no session exists, create one in the workspace root (or current dir)
            if (terminal.GetTerminalCount() == 0)
            {
                std::wstring dir = GetExplorerManager().GetState().rootPath.empty()
                    ? L""
                    : GetExplorerManager().GetState().rootPath;
                terminal.NewTerminal(window_->GetHwnd(), dir);
                // NewTerminal already sets visible & focused
            }
            else
            {
                // Ensure active session initialized (lazy init) via public wrappers
                terminal.EnsureSessionExists(window_->GetHwnd());
                terminal.EnsureActiveInit(window_->GetHwnd());
            }
        }

        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }


    // Ctrl+Shift+O => Open Project
    if (m.ctrl && m.shift && !m.alt && IsLetter(wParam, 'O'))
    {
        Logger::Instance().Log(L"Shortcut: Ctrl+Shift+O Open Project");
        OpenProjectDialog();
        return true;
    }

    // Ctrl+O => Open File
    if (m.ctrl && !m.shift && !m.alt && IsLetter(wParam, 'O'))
    {
        Logger::Instance().Log(L"Shortcut: Ctrl+O Open File");
        OpenFileDialog();
        return true;
    }

    // Ctrl+N => New File (inline)
    if (m.ctrl && !m.shift && !m.alt && IsLetter(wParam, 'N'))
    {
        Logger::Instance().Log(L"Shortcut: Ctrl+N New File");
        if (!GetExplorerManager().IsVisible())
            GetExplorerManager().SetVisible(true);
        GetExplorerManager().ShowInlineInput(Input::Type::File);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // Ctrl+Shift+N => New Folder
    if (m.ctrl && m.shift && !m.alt && IsLetter(wParam, 'N'))
    {
        Logger::Instance().Log(L"Shortcut: Ctrl+Shift+N New Folder");
        if (!GetExplorerManager().IsVisible())
            GetExplorerManager().SetVisible(true);
        GetExplorerManager().ShowInlineInput(Input::Type::Folder);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // Ctrl+W => Close Tab
    if (m.ctrl && !m.shift && !m.alt && IsLetter(wParam, 'W'))
    {
        Logger::Instance().Log(L"Shortcut: Ctrl+W Close Tab");
        CloseActiveTab();
        return true;
    }

    // Ctrl+S => Save
    if (m.ctrl && !m.shift && !m.alt && IsLetter(wParam, 'S'))
    {
        Logger::Instance().Log(L"Shortcut: Ctrl+S Save");
        SaveActiveTab();
        return true;
    }

    // Ctrl+K => start chord (ex: Ctrl+K, Ctrl+D)
    if (m.ctrl && !m.shift && !m.alt && IsLetter(wParam, 'K'))
    {
        StartChord(L'K');
        Footer_SetHint(window_->GetHwnd(), L"Ctrl+K, Ctrl+D : Formater C++", 2000);
        return true;
    }

    // Ctrl+F => Find (editor)
    if (m.ctrl && !m.shift && !m.alt && IsLetter(wParam, 'F'))
    {
        Orion::Editor *editor = window_->GetEditor();
        if (editor)
        {
            editor->ShowSearch();
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Ctrl+Shift+F / Ctrl+Shift+L => Format (editor)
    if (m.ctrl && m.shift && !m.alt && (IsLetter(wParam, 'F') || IsLetter(wParam, 'L')))
    {
        Orion::Editor *editor = window_->GetEditor();
        if (editor)
        {
            Logger::Instance().Log(L"Shortcut: Ctrl+Shift+Format");
            editor->FormatDocument();
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            GetPanelManager().UpdateLayout(window_->GetHwnd());
            return true;
        }
    }

    // Ctrl+Shift+G => Open Source Control panel
    if (m.ctrl && m.shift && !m.alt && IsLetter(wParam, 'G'))
    {
        GetPanelManager().SetActivePanel(PanelId::Git);
        if (Panel *gitPanel = GetPanelManager().GetPanel(PanelId::Git))
            gitPanel->SetVisible(true);
        GetPanelManager().UpdateLayout(window_->GetHwnd());
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    return false;
}

// =====================================================
// Focus routing
// =====================================================

bool KeyboardManager::RouteKeyDownToFocused(WPARAM wParam)
{
    // Explorer inline input
    if (GetExplorerManager().IsInlineInputVisible())
    {
        GetExplorerManager().OnKeyDownInline(wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // Explorer search mode
    if (GetExplorerManager().IsSearchMode())
    {
        GetExplorerManager().OnKeyDownSearch(wParam);

        if (wParam == VK_RETURN)
        {
            const auto &results = GetExplorerManager().GetSearchResults();
            if (!results.empty())
            {
                SendMessageW(window_->GetHwnd(), WM_USER + 100, 0, (LPARAM)results[0].filePath.c_str());
                GetExplorerManager().ExitSearchMode();
            }
        }

        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // SearchPanel focused
    if (GetPanelManager().IsPanelActive(PanelId::Search))
    {
        SearchPanel *searchPanel = GetPanelManager().GetPanelAs<SearchPanel>(PanelId::Search);
        if (searchPanel && searchPanel->IsInputFocused())
        {
            searchPanel->OnKeyDown(wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Marketplace search input
    if (GetPanelManager().IsPanelActive(PanelId::Marketplace))
    {
        MarketplacePanel *marketplacePanel = GetPanelManager().GetPanelAs<MarketplacePanel>(PanelId::Marketplace);
        if (marketplacePanel && marketplacePanel->IsVisible() && marketplacePanel->IsSearchInputFocused())
        {
            marketplacePanel->OnKeyDown(wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Git panel focused input
    if (GetPanelManager().IsPanelActive(PanelId::Git))
    {
        GitPanel *gitPanel = GetPanelManager().GetPanelAs<GitPanel>(PanelId::Git);
        if (gitPanel && gitPanel->IsVisible() && gitPanel->IsInputFocused())
        {
            gitPanel->OnKeyDown(wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Terminal focused
    {
        TerminalPanel &terminal = GetTerminalPanel();
        if (terminal.IsVisible() && terminal.IsInitialized() && terminal.IsFocused())
        {
            terminal.OnKeyDown(wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Fallback editor
    Orion::Editor *editor = window_->GetEditor();
    if (editor)
    {
        editor->OnKeyDown(wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    return false;
}

bool KeyboardManager::RouteCharToFocused(WPARAM wParam)
{
    // PRIORITÃ‰ 0: Explorer inline input
    if (GetExplorerManager().IsInlineInputVisible())
    {
        GetExplorerManager().OnCharInline((wchar_t)wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // PRIORITÃ‰ 0.5: Explorer search mode
    if (GetExplorerManager().IsSearchMode())
    {
        GetExplorerManager().OnCharSearch((wchar_t)wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // PRIORITÃ‰ 1: SearchPanel input
    if (GetPanelManager().IsPanelActive(PanelId::Search))
    {
        SearchPanel *searchPanel = GetPanelManager().GetPanelAs<SearchPanel>(PanelId::Search);
        if (searchPanel && searchPanel->IsInputFocused())
        {
            searchPanel->OnChar((wchar_t)wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Marketplace search input
    if (GetPanelManager().IsPanelActive(PanelId::Marketplace))
    {
        MarketplacePanel *marketplacePanel = GetPanelManager().GetPanelAs<MarketplacePanel>(PanelId::Marketplace);
        if (marketplacePanel && marketplacePanel->IsVisible() && marketplacePanel->IsSearchInputFocused())
        {
            marketplacePanel->OnChar((wchar_t)wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // Git panel focused input
    if (GetPanelManager().IsPanelActive(PanelId::Git))
    {
        GitPanel *gitPanel = GetPanelManager().GetPanelAs<GitPanel>(PanelId::Git);
        if (gitPanel && gitPanel->IsVisible() && gitPanel->IsInputFocused())
        {
            gitPanel->OnChar((wchar_t)wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // PRIORITÃ‰ 1.5: Terminal
    {
        TerminalPanel &terminal = GetTerminalPanel();
        if (terminal.IsVisible() && terminal.IsInitialized() && terminal.IsFocused())
        {
            terminal.OnChar((wchar_t)wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // PRIORITÃ‰ 2: Editor
    Orion::Editor *editor = window_->GetEditor();
    if (editor)
    {
        editor->OnChar((wchar_t)wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    return false;
}
