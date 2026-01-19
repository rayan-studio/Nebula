#include "KeyboardManager.h"

#include "core/window/Window.h"
#include "utils/logger/Logger.h"

#include "core/explorer/Explorer.h"
#include "ui/panels/PanelManager.h"
#include "ui/panels/search/SearchPanel.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "ui/components/input/InputTypeFixed.h"

#include <commdlg.h>
#include <shobjidl.h>
#include <vector>
#include <map>

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

    const Mods m = GetMods();

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

    // WM_CHAR -> route vers l’input actif
    return RouteCharToFocused(wParam);
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

    // delete editor + reindex (même logique que ton code)
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

                // ✅ IMPORTANT: enlever le rond
                window_->GetTabBar()->SetTabDirty(active, false);

                InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            }
        }
    }
    else
    {
        if (editor->SaveToFile(currentPath))
        {
            // ✅ IMPORTANT: enlever le rond
            window_->GetTabBar()->SetTabDirty(active, false);

            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        }
    }
}

// =====================================================
// Global shortcuts
// =====================================================

bool KeyboardManager::HandleGlobalShortcuts(WPARAM wParam, const Mods &m)
{
    // Toggle terminal: Ctrl+`
    if (m.ctrl && !m.shift && !m.alt && (wParam == VK_OEM_3))
    {
        TerminalPanel &terminal = GetTerminalPanel();

        if (!terminal.IsInitialized())
        {
            terminal.Initialize(window_->GetHwnd(), L"");
            terminal.SetFont(L"JetBrains Mono", 13.0f);
        }

        terminal.ToggleVisible();

        // ✅ IMPORTANT : recalculer le layout immédiatement
        GetPanelManager().UpdateLayout(window_->GetHwnd());

        if (terminal.IsVisible())
            terminal.SetFocused(true);

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

    // Ctrl+Shift+F => Format (editor)
    if (m.ctrl && m.shift && !m.alt && IsLetter(wParam, 'F'))
    {
        Orion::Editor *editor = window_->GetEditor();
        if (editor)
        {
            editor->FormatDocument();
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            GetPanelManager().UpdateLayout(window_->GetHwnd());
            return true;
        }
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
    // PRIORITÉ 0: Explorer inline input
    if (GetExplorerManager().IsInlineInputVisible())
    {
        GetExplorerManager().OnCharInline((wchar_t)wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // PRIORITÉ 0.5: Explorer search mode
    if (GetExplorerManager().IsSearchMode())
    {
        GetExplorerManager().OnCharSearch((wchar_t)wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    // PRIORITÉ 1: SearchPanel input
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

    // PRIORITÉ 1.5: Terminal
    {
        TerminalPanel &terminal = GetTerminalPanel();
        if (terminal.IsVisible() && terminal.IsInitialized() && terminal.IsFocused())
        {
            terminal.OnChar((wchar_t)wParam);
            InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
            return true;
        }
    }

    // PRIORITÉ 2: Editor
    Orion::Editor *editor = window_->GetEditor();
    if (editor)
    {
        editor->OnChar((wchar_t)wParam);
        InvalidateRect(window_->GetHwnd(), nullptr, FALSE);
        return true;
    }

    return false;
}
