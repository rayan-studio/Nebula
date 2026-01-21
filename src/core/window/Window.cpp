#include "core/window/Window.h"
#include "ui/graphics/Skia.h"
#include "utils/logger/Logger.h"
#include <sstream>
#include <stdexcept>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <vector>
#include <filesystem>
#include <thread>
#include <uxtheme.h>
#include <vssym32.h>
#include "helpers/window_helpers.h"
#include "ui/components/titlebar/TitleBar.h"
#include "ui/components/popups/CustomPopup.h"
#include "ui/components/footer/Footer.h"
#include "utils/ggwave/ggwave_integration.h"
#include "core/explorer/Explorer.h"
#include "ui/components/sidebar/Sidebar.h"
#include "ui/components/input/InputTypeFixed.h"
#include "ui/panels/PanelInit.h"
#include "ui/panels/PanelManager.h"
#include "ui/panels/search/SearchPanel.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "orion/font/CustomFontLoader.h"
#include <dwrite_1.h>
#include "ui/panels/ggwave/GGWavePanel.h"
#include "utils/logger/Logger.h"
#include "orion/caret/Caret.h"

static void EnableMicaIfAvailable(HWND hwnd)
{
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm)
        return;

    using DwmSetWindowAttribute_t = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    auto pDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttribute_t>(GetProcAddress(hDwm, "DwmSetWindowAttribute"));
    if (!pDwmSetWindowAttribute)
    {
        FreeLibrary(hDwm);
        return;
    }

    BOOL useDark = TRUE;
    pDwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
    pDwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
    const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
    const int DWMSBT_MAINWINDOW = 2;
    pDwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &DWMSBT_MAINWINDOW, sizeof(DWMSBT_MAINWINDOW));

    FreeLibrary(hDwm);
}

static const wchar_t *WINDOW_CLASS_NAME = L"NebulaTextWindowClass";

// Throttled invalidate to avoid excessive redraws on high-frequency events
static DWORD g_lastInvalidateTime = 0;
static void ThrottledInvalidateRect(HWND hwnd, const RECT *rect, BOOL erase)
{
    DWORD now = GetTickCount();
    const DWORD MIN_INTERVAL_MS = 16; // ~60 FPS
    if (now - g_lastInvalidateTime >= MIN_INTERVAL_MS)
    {
        InvalidateRect(hwnd, rect, erase);
        g_lastInvalidateTime = now;
    }
}

static constexpr UINT CARET_TIMER_ID = 1;
static constexpr UINT CARET_TIMER_INTERVAL_MS = 250;

struct KeyMods
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};
static KeyMods GetMods()
{
    KeyMods m;
    m.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    m.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    m.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

static bool KeyIs(WPARAM wParam, int vk)
{
    return (int)wParam == vk;
}

static bool KeyIsChar(WPARAM wParam, wchar_t cUpper)
{
    // WM_KEYDOWN donne virtual-key codes, pour lettres c'est 'A'..'Z'
    return (wParam == (WPARAM)cUpper);
}

static std::wstring QuotePath(const std::filesystem::path &path)
{
    return L"\"" + path.wstring() + L"\"";
}

static bool RunCommandAndWait(const std::wstring &command, const std::filesystem::path &workingDir, DWORD &exitCode)
{
    std::wstring cmdLine = L"cmd.exe /C " + command;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring workdirStr = workingDir.wstring();
    wchar_t *mutableCmd = _wcsdup(cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        workdirStr.empty() ? nullptr : workdirStr.c_str(),
        &si,
        &pi);
    free(mutableCmd);

    if (!ok)
    {
        exitCode = GetLastError();
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static bool LaunchExecutable(const std::filesystem::path &exePath)
{
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring exe = exePath.wstring();
    wchar_t *mutableCmd = _wcsdup(exe.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        exePath.parent_path().wstring().c_str(),
        &si,
        &pi);
    free(mutableCmd);

    if (!ok)
        return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool HasSolutionFile(const std::filesystem::path &root)
{
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        auto ext = entry.path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".sln") == 0)
            return true;
    }
    return false;
}

static std::filesystem::path FindNewestExecutable(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};

    if (!std::filesystem::exists(root, ec))
        return newest;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;

        auto path = entry.path();
        if (_wcsicmp(path.extension().wstring().c_str(), L".exe") != 0)
            continue;

        auto filename = path.filename().wstring();
        if (filename.find(L"cmake") != std::wstring::npos)
            continue;

        auto time = entry.last_write_time(ec);
        if (ec)
            continue;
        if (newest.empty() || time > newestTime)
        {
            newest = path;
            newestTime = time;
        }
    }

    return newest;
}

bool Window::Create(int nCmdShow)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = Window::WndProc;
    wcex.hInstance = hInstance_;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.lpszClassName = WINDOW_CLASS_NAME;
    wcex.hbrBackground = nullptr;
    wcex.hIcon = NULL;
    wcex.hIconSm = NULL;

    HICON hAppIcon = reinterpret_cast<HICON>(LoadImageW(nullptr, L"assets\\favicon.ico", IMAGE_ICON, 32, 32, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    HICON hAppIconSmall = reinterpret_cast<HICON>(LoadImageW(nullptr, L"assets\\favicon.ico", IMAGE_ICON, 16, 16, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    if (hAppIcon)
        wcex.hIcon = hAppIcon;
    if (hAppIconSmall)
        wcex.hIconSm = hAppIconSmall;

    if (!RegisterClassExW(&wcex))
    {
        return false;
    }

    int window_style = WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_VISIBLE;

    int desired_logical_w = 1200;
    int desired_logical_h = 800;

    UINT systemDpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        typedef UINT(WINAPI * GetDpiForSystem_t)();
        auto pGetDpiForSystem = reinterpret_cast<GetDpiForSystem_t>(GetProcAddress(user32, "GetDpiForSystem"));
        if (pGetDpiForSystem)
            systemDpi = pGetDpiForSystem();
    }

    int width_px = MulDiv(desired_logical_w, (int)systemDpi, 96);
    int height_px = MulDiv(desired_logical_h, (int)systemDpi, 96);

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int work_w = workArea.right - workArea.left;
    int work_h = workArea.bottom - workArea.top;
    if (width_px > work_w)
        width_px = work_w;
    if (height_px > work_h)
        height_px = work_h;

    int left = workArea.left + (work_w - width_px) / 2;
    int top = workArea.top + (work_h - height_px) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        WINDOW_CLASS_NAME,
        L"Nebula",
        window_style,
        left,
        top,
        width_px,
        height_px,
        nullptr,
        nullptr,
        hInstance_,
        this);

    if (!hwnd_)
        return false;

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    SetWindowPos(hwnd_, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    EnableMicaIfAvailable(hwnd_);

    return true;
}

int Window::Run()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void Window::SetText(const std::wstring &text)
{
    text_ = text;
    if (skia_)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    Window *self = nullptr;

    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<Window *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else
    {
        self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->HandleMessage(uMsg, wParam, lParam);

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

Orion::Editor *GetOrionEditor(HWND hwnd)
{
    Window *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->GetEditor() : nullptr;
}

Window *GetWindowFromHwnd(HWND hwnd)
{
    return reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

HWND Window::GetHwnd() const
{
    return hwnd_;
}

void Window::CloseEditorForTabIndex(int index)
{
    // Remove and delete the editor for the given tab index, then reindex editors_ to match TabBar
    std::map<int, Orion::Editor *> old = editors_;

    auto it = old.find(index);
    if (it != old.end())
    {
        delete it->second;
        old.erase(it);
    }

    editors_.clear();

    int count = tabBar_.GetTabCount();
    for (int i = 0; i < count; ++i)
    {
        const auto *t = tabBar_.GetTab(i);
        if (!t)
            continue;

        Orion::Editor *found = nullptr;
        for (auto &p : old)
        {
            if (!p.second)
                continue;
            if (p.second->GetFilePath() == t->filePath)
            {
                found = p.second;
                break;
            }
            // Match untitled placeholders
            if (t->filePath.rfind(L"__untitled__", 0) == 0 && p.second->GetFilePath().rfind(L"__untitled__", 0) == 0)
            {
                found = p.second;
                break;
            }
        }

        editors_[i] = found;
        if (found)
        {
            // Ensure the editor's document-changed callback reflects its new tab index
            found->onDocumentChanged = [this, i]()
            {
                tabBar_.SetTabDirty(i, true);
                InvalidateRect(hwnd_, nullptr, FALSE);
            };
        }
    }
}

void Window::RunActiveProject()
{
    std::wstring root = GetExplorerManager().GetState().rootPath;
    if (root.empty())
    {
        MessageBoxW(hwnd_, L"Aucun projet ouvert.", L"Run", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::filesystem::path rootPath(root);
    std::error_code ec;
    bool hasCMake = std::filesystem::exists(rootPath / "CMakeLists.txt", ec);
    bool hasSolution = HasSolutionFile(rootPath);

    if (!hasCMake && !hasSolution)
    {
        MessageBoxW(hwnd_, L"Aucun projet C++ détecté (CMakeLists.txt ou .sln manquant).", L"Run", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!hasCMake)
    {
        MessageBoxW(hwnd_, L"Le lancement automatique supporte seulement CMake pour le moment.", L"Run", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::thread([rootPath, hwnd = hwnd_]()
    {
        Logger::Instance().Log(L"Run: CMake configure/build started.");

        std::filesystem::path buildDir = rootPath / "build";
        DWORD exitCode = 0;
        std::wstring configureCmd = L"cmake -S " + QuotePath(rootPath) + L" -B " + QuotePath(buildDir);
        if (!RunCommandAndWait(configureCmd, rootPath, exitCode))
        {
            Logger::Instance().Log(L"Run: CMake configure failed.");
            MessageBoxW(hwnd, L"Configuration CMake échouée. Vérifie la console.", L"Run", MB_OK | MB_ICONERROR);
            return;
        }

        std::wstring buildCmd = L"cmake --build " + QuotePath(buildDir) + L" --config Debug";
        if (!RunCommandAndWait(buildCmd, rootPath, exitCode))
        {
            Logger::Instance().Log(L"Run: Build failed.");
            MessageBoxW(hwnd, L"Compilation échouée. Vérifie la console.", L"Run", MB_OK | MB_ICONERROR);
            return;
        }

        std::filesystem::path exe = FindNewestExecutable(buildDir / "Debug");
        if (exe.empty())
            exe = FindNewestExecutable(buildDir / "Release");
        if (exe.empty())
            exe = FindNewestExecutable(buildDir);

        if (exe.empty())
        {
            Logger::Instance().Log(L"Run: No executable found after build.");
            MessageBoxW(hwnd, L"Aucun exécutable trouvé après compilation.", L"Run", MB_OK | MB_ICONWARNING);
            return;
        }

        if (!LaunchExecutable(exe))
        {
            Logger::Instance().Log(L"Run: Failed to launch executable.");
            MessageBoxW(hwnd, L"Impossible de lancer l'exécutable.", L"Run", MB_OK | MB_ICONERROR);
            return;
        }

        Logger::Instance().Log(L"Run: Executable launched.");
    }).detach();
}

void Window::HandleCommandLineArgs()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc <= 1)
        return;

    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    LocalFree(argv);

    auto openDirectory = [&](const std::filesystem::path &path)
    {
        std::wstring folder = path.wstring();
        GetExplorerManager().Initialize(folder);
        GetExplorerManager().SetVisible(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    auto openFile = [&](const std::filesystem::path &path)
    {
        std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            openDirectory(parent);
        OpenFileInNewTab(path.wstring(), -1);
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring &arg = args[i];

        if ((arg == L"--project" || arg == L"--folder" || arg == L"-p") && i + 1 < args.size())
        {
            std::filesystem::path path(args[i + 1]);
            if (std::filesystem::exists(path))
                openDirectory(path);
            i++;
            continue;
        }

        if ((arg == L"--file" || arg == L"-f") && i + 1 < args.size())
        {
            std::filesystem::path path(args[i + 1]);
            if (std::filesystem::exists(path))
                openFile(path);
            i++;
            continue;
        }

        std::filesystem::path path(arg);
        if (!std::filesystem::exists(path))
            continue;

        if (std::filesystem::is_directory(path))
        {
            openDirectory(path);
        }
        else
        {
            openFile(path);
        }
    }
}

LRESULT Window::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCCALCSIZE:
    {
        if (!wParam)
            return DefWindowProc(hwnd_, uMsg, wParam, lParam);

        UINT dpi = GetDpiForWindow(hwnd_);
        int frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi);
        int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi);
        int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

        NCCALCSIZE_PARAMS *params = (NCCALCSIZE_PARAMS *)lParam;
        RECT *requested_client_rect = params->rgrc;

        requested_client_rect->right -= frame_x + padding;
        requested_client_rect->left += frame_x + padding;
        requested_client_rect->bottom -= frame_y + padding;

        if (win32_window_is_maximized(hwnd_))
        {
            requested_client_rect->top += frame_y + padding;
        }

        return 0;
    }
    case WM_CREATE:
    {
        skia_ = new Skia();
        if (!skia_->Init(hwnd_))
        {
            delete skia_;
            skia_ = nullptr;
            return -1;
        }
        // Load custom font for all future editors
        {
            wchar_t modulePath[MAX_PATH] = {0};
            if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0)
            {
                std::wstring dir(modulePath);
                size_t pos = dir.find_last_of(L"\\/");
                if (pos != std::wstring::npos)
                    dir = dir.substr(0, pos);

                std::vector<std::wstring> candidates;
                candidates.push_back(dir + L"\\assets\\font\\static\\JetBrainsMono-Regular.ttf");
                candidates.push_back(dir + L"\\..\\assets\\font\\static\\JetBrainsMono-Regular.ttf");
                candidates.push_back(dir + L"\\..\\..\\assets\\font\\static\\JetBrainsMono-Regular.ttf");

                for (const auto &cand : candidates)
                {
                    wchar_t full[MAX_PATH] = {0};
                    if (GetFullPathNameW(cand.c_str(), MAX_PATH, full, NULL) > 0)
                    {
                        DWORD attr = GetFileAttributesW(full);
                        if (attr != INVALID_FILE_ATTRIBUTES)
                        {
                            customFontPath_ = full;
                            break;
                        }
                    }
                }
            }
        // If we found a custom font path, try to create a custom font collection
        if (!customFontPath_.empty() && skia_)
        {
            IDWriteFactory *dwrite = skia_->GetDWriteFactory();
            if (dwrite)
            {
                IDWriteFactory1 *factory1 = nullptr;
                if (SUCCEEDED(dwrite->QueryInterface(__uuidof(IDWriteFactory1), (void **)&factory1)))
                {
                    CustomFontCollectionLoader *fontLoader = new CustomFontCollectionLoader();
                    HRESULT regHr = dwrite->RegisterFontCollectionLoader(fontLoader);
                    if (SUCCEEDED(regHr))
                    {
                        const void *collectionKey = customFontPath_.c_str();
                        UINT32 collectionKeySize = (UINT32)((customFontPath_.size() + 1) * sizeof(wchar_t));
                        IDWriteFontCollection *fontCollection = nullptr;
                        HRESULT hr = factory1->CreateCustomFontCollection(fontLoader, collectionKey, collectionKeySize, &fontCollection);
                        if (SUCCEEDED(hr) && fontCollection)
                        {
                            GetTerminalPanel().SetFont(L"JetBrains Mono", 13.0f);
                            GetTerminalPanel().SetFontCollection(fontCollection);
                        }
                        else
                        {
                            // If creation failed, unregister loader and release it
                            dwrite->UnregisterFontCollectionLoader(fontLoader);
                            fontLoader->Release();
                        }
                    }
                    factory1->Release();
                }
            }
        }
        }
        SetText(L"Bonjour — texte rendu via GPU (Direct2D)");

        SetTimer(hwnd_, CARET_TIMER_ID, CARET_TIMER_INTERVAL_MS, nullptr);

        // Initialize ggwave wrapper (will fallback to SAPI if not enabled)
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        ggwave::Initialize();

        // Initialize the panel system
        InitializePanelSystem();
        // Initialize keyboard manager when HWND is available
        keyboard_.Init(this);
        HandleCommandLineArgs();

        RECT clientRect;
        GetClientRect(hwnd_, &clientRect);
        UINT dpiInit = GetDpiForWindow(hwnd_);
        UINT initW = MulDiv(clientRect.right - clientRect.left, dpiInit, 96);
        UINT initH = MulDiv(clientRect.bottom - clientRect.top, dpiInit, 96);
        skia_->Resize(initW, initH);

        SetWindowPos(hwnd_, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_ACTIVATE:
    {
        RECT title_bar_rect = win32_titlebar_rect(hwnd_);
        // Clear hovered state when window activation changes
        hoveredButton_ = Hovered_None;
        for (int i = 0; i < 8; ++i)
            SetMenuItemHovered(i, false);
        HideMenuDropdown(hwnd_);
        InvalidateRect(hwnd_, &title_bar_rect, FALSE);
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProc(hwnd_, uMsg, wParam, lParam);
        switch (hit)
        {
        case HTNOWHERE:
        case HTRIGHT:
        case HTLEFT:
        case HTTOPLEFT:
        case HTTOP:
        case HTTOPRIGHT:
        case HTBOTTOMRIGHT:
        case HTBOTTOM:
        case HTBOTTOMLEFT:
            return hit;
        }

        auto title_bar_hovered_button = hoveredButton_;

        if (title_bar_hovered_button == Window::Hovered_Maximize)
        {
            return HTMAXBUTTON;
        }

        POINT cursor_point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &cursor_point);

        int hoveredMenu = GetHoveredMenuItem(hwnd_, cursor_point);
        if (hoveredMenu >= 0)
        {
            return HTCLIENT;
        }

        if (cursor_point.y < win32_titlebar_rect(hwnd_).bottom)
        {
            return HTCAPTION;
        }

        return HTCLIENT;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);

        bool has_focus = !!GetFocus();
        int hovered = static_cast<int>(hoveredButton_);
        wchar_t title_text_buffer[255] = {0};
        GetWindowTextW(hwnd_, title_text_buffer, (int)std::size(title_text_buffer));

        if (skia_)
        {
            skia_->Render(text_, hwnd_, hovered, has_focus, std::wstring(title_text_buffer), ps.hdc);
        }

        // Inline input is drawn by Explorer::DrawItems when visible; no global overlay drawing here.

        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE:
    {
        if (skia_)
        {
            RECT clientRect;
            GetClientRect(hwnd_, &clientRect);
            UINT dpi = GetDpiForWindow(hwnd_);
            UINT w = MulDiv(clientRect.right - clientRect.left, dpi, 96);
            UINT h = MulDiv(clientRect.bottom - clientRect.top, dpi, 96);
            skia_->Resize(w, h);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_NCMOUSEMOVE:
    {
        POINT cursor_point;
        GetCursorPos(&cursor_point);
        ScreenToClient(hwnd_, &cursor_point);

        RECT title_bar_rect = win32_titlebar_rect(hwnd_);

        int hoveredMenu = GetHoveredMenuItem(hwnd_, cursor_point);
        for (int i = 0; i < 8; i++)
        {
            SetMenuItemHovered(i, i == hoveredMenu);
        }

        CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd_, &title_bar_rect);

        Window::CustomTitleBarHoveredButton new_hovered_button = Window::Hovered_None;
        if (PtInRect(&button_rects.close, cursor_point))
        {
            new_hovered_button = Window::Hovered_Close;
        }
        else if (PtInRect(&button_rects.minimize, cursor_point))
        {
            new_hovered_button = Window::Hovered_Minimize;
        }
        else if (PtInRect(&button_rects.maximize, cursor_point))
        {
            new_hovered_button = Window::Hovered_Maximize;
        }
        else if (PtInRect(&button_rects.run, cursor_point))
        {
            new_hovered_button = Window::Hovered_Run;
        }
        auto current = hoveredButton_;
        if (new_hovered_button != current)
        {
            InvalidateRect(hwnd_, &button_rects.close, FALSE);
            InvalidateRect(hwnd_, &button_rects.minimize, FALSE);
            InvalidateRect(hwnd_, &button_rects.maximize, FALSE);
            InvalidateRect(hwnd_, &button_rects.run, FALSE);
            hoveredButton_ = new_hovered_button;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_EDITOR_FILE_LOADED:
    {
        auto *res = reinterpret_cast<EditorFileLoadResult *>(lParam);
        if (!res)
            return 0;

        Orion::Editor *ed = GetEditorForTab(res->tabIndex);
        if (ed)
        {
            // IMPORTANT: vérifier que l’éditeur correspond toujours au même fichier (tab peut avoir changé)
            if (ed->GetFilePath() == res->filePath)
            {
                ed->ApplyLoadedFile(std::move(res->filePath), std::move(res->encoding), std::move(res->lines));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }

        delete res;
        return 0;
    }

    case WM_USER + 100:
    {
        auto *pPath = reinterpret_cast<std::wstring *>(lParam);
        int lineNumber = (int)wParam;

        if (pPath)
        {
            OpenFileInNewTab(*pPath, lineNumber > 0 ? lineNumber : -1);
            delete pPath;
        }
        return 0;
    }

    case WM_USER + 201:
    {
        // Posted from TerminalPanel::ReadThread - lParam is heap buffer, wParam is length
        char *buf = reinterpret_cast<char *>(lParam);
        size_t len = (size_t)wParam;

        if (buf && len > 0)
        {
            TerminalPanel &terminal = GetTerminalPanel();
            terminal.HandleConPTYOutput(buf, len);
        }

        if (buf)
            free(buf);

        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    
    case WM_LBUTTONDOWN:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // If a titlebar menu dropdown is visible, let it handle clicks first
        if (IsMenuDropdownVisible())
        {
            int itemIndex = GetDropdownHoveredItem(pt);
            if (itemIndex >= 0)
            {
                int baseId = GetActiveDropdown().baseId;
                if (baseId >= 5000 && baseId < 6000)
                {
                    int commandId = baseId + itemIndex;
                    GetExplorerManager().HandleContextCommand(commandId);
                }
                else if (baseId >= 7000 && baseId < 8000)
                {
                    int commandId = baseId + itemIndex;
                    PostMessageW(hwnd_, WM_COMMAND, commandId, 0);
                }
                else
                {
                    int menuIndex = GetActiveDropdown().menuIndex;
                    int commandId = 3000 + menuIndex * 100 + itemIndex;
                    PostMessageW(hwnd_, WM_COMMAND, commandId, 0);
                }
                HideMenuDropdown(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (!IsPointInDropdown(pt))
            {
                HideMenuDropdown(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            else
            {
                return 0;
            }
        }

        // Global Input overlay removed; clicks always propagate to Explorer/editor.

        // If click is inside Explorer, forward it first
        if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
        {
            GetExplorerManager().OnLeftButtonDown(hwnd_, pt);
            return 0;
        }

        // Sidebar icon clicks (toggle explorer)
        if (HandleSidebarLeftClick(hwnd_, pt))
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        RECT tbRect = win32_titlebar_rect(hwnd_);
        if (pt.y >= tbRect.bottom && pt.y < tbRect.bottom + tabBar_.GetHeight())
        {
            int r = tabBar_.OnLeftButtonDown(pt);

            if (r != -1) // ✅ tab activated OR close requested => event consumed
            {
                // Ensure any editor mouse interactions are cancelled and release capture
                ReleaseCapture();
                Orion::Editor *editor = GetEditor();
                if (editor)
                    editor->CancelInteraction();

                // Handle close request separately so Window decides (UI only from TabBar)
                if (r == TabBar::TAB_CLICKED_CLOSE)
                {
                    int idx = tabBar_.GetLastCloseRequestIndex();
                    if (idx >= 0)
                    {
                        if (tabBar_.IsTabDirty(idx))
                        {
                            int choice = MessageBoxW(
                                hwnd_,
                                L"Le fichier n'est pas sauvegardé.\n\nOui = Enregistrer et fermer\nNon = Fermer sans enregistrer\nAnnuler = Annuler",
                                L"Fichier modifié",
                                MB_YESNOCANCEL | MB_ICONWARNING);

                            if (choice == IDYES)
                            {
                                Orion::Editor *ed = GetEditorForTab(idx);
                                if (ed)
                                {
                                    bool ok = ed->SaveToFile(ed->GetFilePath());
                                    if (ok)
                                    {
                                        tabBar_.SetTabDirty(idx, false);
                                        tabBar_.CloseTab(idx);
                                        CloseEditorForTabIndex(idx);
                                    }
                                }
                            }
                            else if (choice == IDNO)
                            {
                                tabBar_.CloseTab(idx);
                                CloseEditorForTabIndex(idx);
                            }
                            else
                            {
                                // Cancel: do nothing
                            }
                        }
                        else
                        {
                            tabBar_.CloseTab(idx);
                            CloseEditorForTabIndex(idx);
                        }

                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                }

                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        int hoveredMenu = GetHoveredMenuItem(hwnd_, pt);
        if (hoveredMenu >= 0)
        {
            if (IsMenuDropdownVisible() && GetActiveDropdown().menuIndex == hoveredMenu)
            {
                HideMenuDropdown(hwnd_);
            }
            else
            {
                D2D1_RECT_F menuRect = GetMenuItems()[hoveredMenu].rect;
                ShowMenuDropdown(hwnd_, hoveredMenu, menuRect);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // Check if click is in active panel area
        Panel *activePanel = GetPanelManager().GetActivePanel();
        bool inPanel = false;
        bool inResizeZone = false;

        if (activePanel && activePanel->IsVisible())
        {
            inPanel = activePanel->IsPointInPanel(pt);
            inResizeZone = activePanel->IsPointInResizeZone(pt);
        }

        if (inPanel || inResizeZone)
        {
            // Cancel editor interactions before panel takes capture (resize/scroll)
            ReleaseCapture();
            Orion::Editor *editor = GetEditor();
            if (editor)
                editor->CancelInteraction();

            GetPanelManager().OnLeftButtonDown(hwnd_, pt);
            return 0;
        }

        // Check if click is on GGWave button
        GGWavePanel &ggwave = GetGGWavePanel();
        if (ggwave.IsPointOnButton(pt))
        {
            ggwave.OnLeftButtonDown(hwnd_, pt);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // Check if click is in terminal area
        TerminalPanel &terminal = GetTerminalPanel();
        if (terminal.IsVisible())
        {
            bool inTerminal = terminal.IsPointInPanel(pt);
            bool inTerminalResize = terminal.IsPointInResizeZone(pt);

            if (inTerminal || inTerminalResize)
            {
                ReleaseCapture();
                Orion::Editor *editor = GetEditor();
                if (editor)
                    editor->CancelInteraction();

                terminal.OnLeftButtonDown(hwnd_, pt);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            else
            {
                // Click outside terminal - unfocus it
                terminal.Unfocus();
            }
        }

        Orion::Editor *editor = GetEditor();
        if (editor)
        {
            // Clicking in editor area - unfocus SearchPanel input if it was focused
            if (GetPanelManager().IsPanelActive(PanelId::Search))
            {
                SearchPanel *searchPanel = GetPanelManager().GetPanelAs<SearchPanel>(PanelId::Search);
                if (searchPanel && searchPanel->IsInputFocused())
                {
                    searchPanel->UnfocusInput();
                }
            }

            // Unfocus terminal when clicking in editor
            GetTerminalPanel().Unfocus();

            editor->OnLeftButtonDown(hwnd_, pt);
            // Capture the mouse so we continue receiving mouse events
            // even when the cursor leaves the window while dragging.
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_LBUTTONDBLCLK:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // PRIORITÉ : Explorer double-click
        if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
        {
            GetExplorerManager().OnLeftButtonDoubleClick(hwnd_, pt);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        RECT tbRect = win32_titlebar_rect(hwnd_);
        RECT clientRect;
        GetClientRect(hwnd_, &clientRect);
        // If double-click occurred inside the editor area (not in panel/sidebar)
        // and there is no active tab, create a new empty untitled tab.
        UINT dpi = GetDpiForWindow(hwnd_);
        int sidebarWidth = win32_dpi_scale(50, dpi);

        // Get panel width from active panel
        Panel *panelDblClick = GetPanelManager().GetActivePanel();
        int panelWidth = (panelDblClick && panelDblClick->IsVisible()) ? panelDblClick->GetPhysicalWidth() : 0;
        int editorLeftX = sidebarWidth + panelWidth;

        bool inPanelDbl = panelDblClick && panelDblClick->IsVisible() &&
                          (panelDblClick->IsPointInPanel(pt) || panelDblClick->IsPointInResizeZone(pt));

        if (tabBar_.GetActiveTabIndex() < 0 &&
            pt.y >= tbRect.bottom + tabBar_.GetHeight() && pt.y <= clientRect.bottom &&
            pt.x >= editorLeftX && pt.x <= clientRect.right &&
            !inPanelDbl)
        {
            // create unique untitled placeholder path
            wchar_t placeholder[64];
            swprintf_s(placeholder, L"__untitled__#%d", untitledCounter_++);
            std::wstring displayName = L"Untitled";
            int tabIndex = tabBar_.AddTab(placeholder, displayName);

            if (editors_.count(tabIndex) == 0)
            {
                Orion::Editor *newEditor = new Orion::Editor();
                if (!customFontPath_.empty() && skia_)
                {
                    newEditor->LoadCustomFont(skia_->GetDWriteFactory(), customFontPath_);
                }
                newEditor->CreateEmpty();
                editors_[tabIndex] = newEditor;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_CHAR:
    {
        if (keyboard_.OnChar(wParam))
            return 0;

        return 0;
    }
    case WM_KEYDOWN:
    {
        // ✅ Toute la logique est dans KeyboardManager
        if (keyboard_.OnKeyDown(wParam))
            return 0;

        // fallback
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &pt);

        // If wheel is over Explorer, let it handle the scroll
        if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            GetExplorerManager().OnMouseWheel(hwnd_, delta);
            return 0;
        }

        // Check if wheel is over active panel
        Panel *panelWheel = GetPanelManager().GetActivePanel();
        if (panelWheel && panelWheel->IsVisible() && panelWheel->IsPointInPanel(pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            GetPanelManager().OnMouseWheel(hwnd_, delta);
            return 0;
        }

        // Check if wheel is over terminal
        TerminalPanel &terminalWheel = GetTerminalPanel();
        if (terminalWheel.IsVisible() && terminalWheel.IsPointInPanel(pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            terminalWheel.OnMouseWheel(hwnd_, delta);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // Route wheel to editor when not over panel (pass Ctrl state for zoom)
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            // ✨ Détecter si Ctrl est pressé
            bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

            Orion::Editor *editor = GetEditor();
            if (editor)
            {
                editor->OnMouseWheel(hwnd_, delta, ctrlPressed);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_MOUSEHWHEEL:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &pt);

        // Route horizontal wheel to editor (do not forward to Explorer - explorer is vertical list)
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            Orion::Editor *editor = GetEditor();
            if (editor)
            {
                editor->OnHorizontalWheel(hwnd_, delta);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // Ensure we get WM_MOUSELEAVE when the cursor exits the window
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);

        // ============================================================
        // ✅ TABBAR HOVER: always update first (anti hover "stuck")
        // ============================================================
        bool tabChanged = UpdateTabBarHover(pt);
        if (tabChanged)
        {
            RECT tabRect = GetTabBarRectClient();
            InvalidateRect(hwnd_, &tabRect, FALSE); // not throttled for hover
        }

        // If we're inside the tabbar -> consume the event here
        RECT tabRect = GetTabBarRectClient();
        bool inTabBar = (pt.x >= tabRect.left && pt.x < tabRect.right &&
                         pt.y >= tabRect.top  && pt.y < tabRect.bottom);
        if (inTabBar)
        {
            // Optional: avoid explorer hover while over tabbar
            GetExplorerManager().ClearHover(hwnd_);
            return 0;
        }

        // ============================================================
        // Rest: normal routing
        // ============================================================
        bool needsRedraw = false;

        // Check if active panel is resizing
        Panel* activePanel = GetPanelManager().GetActivePanel();
        bool panelResizing = activePanel && activePanel->IsResizing();
        if (panelResizing)
        {
            GetPanelManager().OnMouseMove(hwnd_, pt);
            return 0;
        }

        bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

        // ===== TERMINAL HANDLING - Optimized =====
        TerminalPanel& terminal = GetTerminalPanel();
        if (terminal.IsVisible())
        {
            // Priority 1: resizing
            if (terminal.IsResizing())
            {
                bool changed = terminal.OnMouseMove(hwnd_, pt);
                if (changed)
                {
                    const auto& st = terminal.GetState();
                    RECT tr = {(LONG)st.leftEdge, (LONG)st.topEdge, (LONG)st.rightEdge, (LONG)st.bottomEdge};
                    ThrottledInvalidateRect(hwnd_, &tr, FALSE);
                }
                return 0;
            }

            // Priority 2: hover resize zone => just cursor
            if (terminal.IsPointInResizeZone(pt))
            {
                terminal.OnMouseMove(hwnd_, pt);
                return 0;
            }

            // Priority 3: selection drag
            if (lmbDown && terminal.IsPointInPanel(pt))
            {
                bool changed = terminal.OnMouseMove(hwnd_, pt);
                if (changed)
                {
                    const auto& st = terminal.GetState();
                    RECT tr = {(LONG)st.leftEdge, (LONG)st.topEdge, (LONG)st.rightEdge, (LONG)st.bottomEdge};
                    ThrottledInvalidateRect(hwnd_, &tr, FALSE);
                }
                return 0;
            }
        }
        // ===== END TERMINAL HANDLING =====

        // GGWave button hover
        GetGGWavePanel().OnMouseMove(hwnd_, pt);

        // If left button is down, prioritize editor dragging selection
        if (lmbDown)
        {
            Orion::Editor* editor = GetEditor();
            if (editor)
            {
                editor->OnMouseMove(hwnd_, pt);
                ThrottledInvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        // Menu dropdown handling
        if (IsMenuDropdownVisible())
        {
            int hoveredItem = GetDropdownHoveredItem(pt);
            if (hoveredItem != GetActiveDropdown().hoveredItem)
            {
                SetDropdownHoveredItem(hoveredItem);
                needsRedraw = true;
            }

            int originMenu = GetActiveDropdown().menuIndex;
            for (size_t i = 0; i < GetMenuItems().size(); ++i)
                SetMenuItemHovered((int)i, (int)i == originMenu);

            if (needsRedraw)
                ThrottledInvalidateRect(hwnd_, nullptr, FALSE);

            return 0;
        }

        // Title bar menu hover
        RECT title_bar_rect = win32_titlebar_rect(hwnd_);
        if (pt.y < title_bar_rect.bottom)
        {
            GetPanelManager().ClearResizeHover(hwnd_);

            int hoveredMenu = GetHoveredMenuItem(hwnd_, pt);
            static int lastHoveredMenu = -1;
            if (hoveredMenu != lastHoveredMenu)
            {
                for (int i = 0; i < 8; ++i)
                    SetMenuItemHovered(i, i == hoveredMenu);
                needsRedraw = true;
                lastHoveredMenu = hoveredMenu;
            }
        }
        else
        {
            Panel* activePnl = GetPanelManager().GetActivePanel();
            bool inPanelArea = false;
            bool inPanelResize = false;

            if (activePnl && activePnl->IsVisible())
            {
                inPanelArea = activePnl->IsPointInPanel(pt);
                inPanelResize = activePnl->IsPointInResizeZone(pt);
            }

            if (inPanelArea || inPanelResize)
            {
                GetPanelManager().OnMouseMove(hwnd_, pt);
            }
            else
            {
                RECT client;
                GetClientRect(hwnd_, &client);
                UINT dpi = GetDpiForWindow(hwnd_);
                int footerLogicalH = 28;
                int footerH = win32_dpi_scale(footerLogicalH, dpi);

                if (pt.y >= client.bottom - footerH)
                {
                    Footer_OnMouseMove(hwnd_, pt);
                    return 0;
                }

                ClearAllHoverStates();

                // Explorer hover
                if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
                {
                    GetExplorerManager().OnMouseMove(hwnd_, pt);
                }
                else
                {
                    Orion::Editor* editor = GetEditor();
                    if (editor)
                        editor->OnMouseMove(hwnd_, pt);
                }
            }
        }

        if (needsRedraw)
            ThrottledInvalidateRect(hwnd_, nullptr, FALSE);

        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // Check if any panel is resizing
        Panel *activePanelUp = GetPanelManager().GetActivePanel();
        bool panelWasResizing = activePanelUp && activePanelUp->IsResizing();

        if (panelWasResizing)
        {
            GetPanelManager().OnLeftButtonUp(hwnd_);
            return 0;
        }

        // Check if terminal was resizing
        TerminalPanel &terminalUp = GetTerminalPanel();
        if (terminalUp.IsVisible() && terminalUp.IsResizing())
        {
            terminalUp.OnLeftButtonUp(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // If terminal had captured the mouse (selection/drag), ensure it gets the mouse-up
        // This ends selection properly; otherwise selection_.selecting can remain true
        // and cause continuous redraws on mouse move.
        if (terminalUp.IsVisible() && GetCapture() == hwnd_)
        {
            terminalUp.OnLeftButtonUp(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // GGWave button release
        GGWavePanel &ggwaveUp = GetGGWavePanel();
        ggwaveUp.OnLeftButtonUp(hwnd_);

        int hoveredMenu = GetHoveredMenuItem(hwnd_, pt);

        if (hoveredMenu >= 0)
        {
            {
                std::wstringstream ss;
                ss << L"Window::WM_LBUTTONUP - hoveredMenu=" << hoveredMenu << L" (client pt=" << pt.x << L"," << pt.y << L")";
                Logger::Instance().Log(ss.str());
            }
            POINT screenPt = pt;
            ClientToScreen(hwnd_, &screenPt);

            std::vector<std::wstring> popupItems;
            switch (hoveredMenu)
            {
            case 0:
                popupItems = {L"New", L"Open", L"Save", L"Close"};
                break;
            case 1:
                popupItems = {L"Undo", L"Redo", L"Cut", L"Copy", L"Paste"};
                break;
            case 6:
                popupItems = {L"New Terminal", L"Split Terminal", L"Kill Terminal"};
                break;
            default:
                popupItems = {L"Item 1", L"Item 2", L"Item 3"};
                break;
            }

            RECT title_bar_rect = win32_titlebar_rect(hwnd_);
            screenPt.y += (title_bar_rect.bottom - title_bar_rect.top);

            ShowCustomPopup(hwnd_, popupItems, screenPt, 3000 + hoveredMenu * 100);
            return 0;
        }

        // If click wasn't on a menu popup, forward to Explorer or editor to handle mouse-up
        if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
        {
            GetExplorerManager().OnLeftButtonUp(hwnd_);
            return 0;
        }
        else
        {
            Orion::Editor *editor = GetEditor();
            if (editor)
            {
                // Release mouse capture when mouse button is released
                ReleaseCapture();
                editor->OnLeftButtonUp(hwnd_, pt);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_MOUSELEAVE:
    {
        // Clear all hover states when mouse leaves the window entirely
        ClearAllHoverStates();

        // ✅ guarantees tabbar hover is clean
        tabBar_.ClearHover();
        RECT tabRect = GetTabBarRectClient();
        InvalidateRect(hwnd_, &tabRect, FALSE);

        return 0;
    }
    case WM_TIMER:
    {
        if (wParam == CARET_TIMER_ID)
        {
            Orion::Editor *editor = GetEditor();
            if (editor && editor->UpdateCaretBlink())
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        // Editor context menu commands (7000..7999)
        if (id >= 7000 && id < 8000)
        {
            int rel = id - 7000;
            int cmdIndex = rel; // single-menu
            Orion::Editor *editor = GetEditor();
            if (editor)
            {
                // 0: Couper, 1: Copier, 2: Coller, 3: Envoyer avec ggwave
                switch (cmdIndex)
                {
                case 0: // Couper
                    editor->CutSelectionToClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 1: // Copier
                    editor->CopySelectionToClipboard();
                    return 0;
                case 2: // Coller
                    editor->PasteFromClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 3: // Envoyer avec ggwave
                {
                    std::wstring sel = editor->GetSelectionText();
                    if (!sel.empty())
                    {
                        ggwave::SpeakText(sel);
                        return 0;
                    }
                }
                break;
                }
            }
        }

        if (id >= 5000 && id < 6000)
        {
            GetExplorerManager().HandleContextCommand(id);
            return 0;
        }

        if (id >= 3000 && id < 4000)
        {
            int rel = id - 3000;
            int menu = rel / 100;
            int index = rel % 100;
            // Menu handling: implement core File menu actions and keep existing stubs for others.
            if (menu == 0)
            {
                // File menu simplified: New, New Window, Open..., Close
                switch (index)
                {
                case 0: // New -> create untitled tab
                {
                    wchar_t placeholder[64];
                    swprintf_s(placeholder, L"__untitled__#%d", untitledCounter_++);
                    std::wstring displayName = L"Untitled";
                    int tabIndex = tabBar_.AddTab(placeholder, displayName);

                    if (editors_.count(tabIndex) == 0)
                    {
                        Orion::Editor *editor = new Orion::Editor();

                        if (!customFontPath_.empty() && skia_)
                            editor->LoadCustomFont(skia_->GetDWriteFactory(), customFontPath_);

                        // "New" should create an empty editor
                        editor->CreateEmpty();

                        editors_[tabIndex] = editor;
                    }

                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                case 1: // New Window -> spawn new process
                {
                    wchar_t exePath[MAX_PATH] = {0};
                    GetModuleFileNameW(NULL, exePath, MAX_PATH);
                    STARTUPINFOW si = {sizeof(si)};
                    PROCESS_INFORMATION pi = {};
                    if (CreateProcessW(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
                    {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                    return 0;
                }
                case 2: // Open... -> file open dialog
                {
                    wchar_t fileName[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd_;
                    ofn.lpstrFile = fileName;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = L"All Files\0*.*\0Text Files\0*.txt\0\0";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn))
                    {
                        OpenFileInNewTab(std::wstring(fileName), -1);
                    }
                    return 0;
                }
                case 3: // Open Project -> pick folder and initialize Explorer
                {
                    IFileOpenDialog *pFileOpen = nullptr;
                    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
                    if (SUCCEEDED(hr) && pFileOpen)
                    {
                        DWORD options = 0;
                        if (SUCCEEDED(pFileOpen->GetOptions(&options)))
                        {
                            pFileOpen->SetOptions(options | FOS_PICKFOLDERS);
                        }
                        if (SUCCEEDED(pFileOpen->Show(hwnd_)))
                        {
                            IShellItem *pItem = nullptr;
                            if (SUCCEEDED(pFileOpen->GetResult(&pItem)) && pItem)
                            {
                                PWSTR pszPath = nullptr;
                                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
                                {
                                    std::wstring selectedFolder = pszPath;
                                    CoTaskMemFree(pszPath);
                                    // Initialize Explorer to this folder (open project)
                                    GetExplorerManager().Initialize(selectedFolder);
                                    GetExplorerManager().SetVisible(true);
                                    InvalidateRect(hwnd_, nullptr, FALSE);
                                }
                                pItem->Release();
                            }
                        }
                        pFileOpen->Release();
                    }
                    return 0;
                }
                case 4: // Close -> close active tab
                {
                    int active = tabBar_.GetActiveTabIndex();
                    if (active >= 0)
                    {
                        tabBar_.CloseTab(active);
                        auto it = editors_.find(active);
                        if (it != editors_.end())
                        {
                            delete it->second;
                            editors_.erase(it);
                        }

                        if (!editors_.empty())
                        {
                            std::map<int, Orion::Editor *> newEditors;
                            for (auto &p : editors_)
                            {
                                int key = p.first;
                                Orion::Editor *ed = p.second;
                                if (key > active)
                                    newEditors[key - 1] = ed;
                                else
                                    newEditors[key] = ed;
                            }
                            editors_.swap(newEditors);
                        }
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
                default:
                    break;
                }
            }

            // Dynamic Edit menu wired to Orion::Editor where possible
            if (menu == 1)
            {
                Orion::Editor *editor = GetEditor();
                if (!editor)
                {
                    MessageBoxW(hwnd_, L"No active editor", L"Edit", MB_OK);
                    return 0;
                }

                switch (index)
                {
                case 0: // Undo
                    editor->Undo();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 1: // Redo (not implemented)
                    MessageBoxW(hwnd_, L"Redo not implemented", L"Edit", MB_OK);
                    return 0;
                case 2: // Cut
                    editor->CutSelectionToClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 3: // Copy
                    editor->CopySelectionToClipboard();
                    return 0;
                case 4: // Paste
                    editor->PasteFromClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 5: // Paste Without Formatting -> fallback to Paste
                    editor->PasteFromClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 6: // Delete
                    editor->DeleteSelectionPublic();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 7: // Select All
                    editor->SelectAll();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 8: // Find
                    editor->ShowSearch();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 9: // Replace
                    MessageBoxW(hwnd_, L"Replace not implemented", L"Edit", MB_OK);
                    return 0;
                case 10: // Find in Files
                    MessageBoxW(hwnd_, L"Find in Files not implemented", L"Edit", MB_OK);
                    return 0;
                case 11: // Replace in Files
                    MessageBoxW(hwnd_, L"Replace in Files not implemented", L"Edit", MB_OK);
                    return 0;
                case 12: // Toggle Comment
                    MessageBoxW(hwnd_, L"Toggle Comment not implemented", L"Edit", MB_OK);
                    return 0;
                case 13: // Format Document
                {
                    Orion::Editor *editor = GetEditor();
                    if (editor)
                    {
                        editor->FormatDocument();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        GetPanelManager().UpdateLayout(hwnd_);
                    }
                    return 0;
                }
                default:
                    break;
                }
            }

            wchar_t buf[256];
            swprintf_s(buf, sizeof(buf) / sizeof(buf[0]), L"Menu %d item %d selected", menu, index);
            // Handle some View menu actions (menu == 3)
            if (menu == 3)
            {
                switch (index)
                {
                case 0: // New Terminal
                {
                    std::wstring dir = GetExplorerManager().GetState().rootPath.empty()
                        ? L""
                        : GetExplorerManager().GetState().rootPath;
                    GetTerminalPanel().NewTerminal(hwnd_, dir);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                default:
                    break;
                }
            }

            MessageBoxW(hwnd_, buf, L"Menu", MB_OK);
            return 0;
        }
        break;
    }
    case WM_NCLBUTTONDOWN:
    {
        auto current = hoveredButton_;
        if (current != Window::Hovered_None)
        {
            return 0;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_NCLBUTTONUP:
    {
        auto current = hoveredButton_;
        if (current == Window::Hovered_Close)
        {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return 0;
        }
        else if (current == Window::Hovered_Minimize)
        {
            ShowWindow(hwnd_, SW_MINIMIZE);
            return 0;
        }
        else if (current == Window::Hovered_Maximize)
        {
            int mode = win32_window_is_maximized(hwnd_) ? SW_NORMAL : SW_MAXIMIZE;
            ShowWindow(hwnd_, mode);
            return 0;
        }
        else if (current == Window::Hovered_Run)
        {
            RunActiveProject();
            return 0;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_RBUTTONUP:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // Check if right-click is in active panel
        Panel *panelRClick = GetPanelManager().GetActivePanel();
        if (panelRClick && panelRClick->IsVisible() && panelRClick->IsPointInPanel(pt))
        {
            GetPanelManager().OnRightButtonUp(hwnd_, pt);
            return 0;
        }

        // If click is not in panel, check editor selection and show context menu
        Orion::Editor *editor = GetEditor();
        if (editor)
        {
            // Build context menu in French: Couper, Copier, Coller, Envoyer avec ggwave
            std::vector<std::wstring> items = {L"Couper", L"Copier", L"Coller", L"Envoyer avec ggwave"};
            D2D1_POINT_2F pos = D2D1::Point2F((float)pt.x, (float)pt.y);
            ShowContextMenuDropdown(hwnd_, items, pos, 7000);

            // Set enabled flags: Cut/Copy enabled only if selection exists; Paste enabled only if clipboard has text
            std::vector<bool> enabled(items.size(), true);
            std::wstring sel = editor->GetSelectionText();
            bool hasSelection = !sel.empty();
            enabled[0] = hasSelection; // Couper
            enabled[1] = hasSelection; // Copier

            // Check clipboard for Unicode text
            bool canPaste = false;
            if (OpenClipboard(NULL))
            {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData)
                {
                    wchar_t *clip = static_cast<wchar_t *>(GlobalLock(hData));
                    if (clip)
                    {
                        std::wstring txt(clip);
                        if (!txt.empty())
                            canPaste = true;
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            enabled[2] = canPaste;     // Coller
            enabled[3] = hasSelection; // Envoyer avec ggwave

            // Apply to active dropdown
            MenuDropdown &dd = GetActiveDropdown();
            dd.enabled.clear();
            dd.enabled = enabled;

            return 0;
        }

        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_NCRBUTTONUP:
    {
        if (wParam == HTCAPTION)
        {
            BOOL const isMaximized = IsZoomed(hwnd_);
            MENUITEMINFO menu_item_info = {};
            menu_item_info.cbSize = sizeof(menu_item_info);
            menu_item_info.fMask = MIIM_STATE;
            HMENU const sys_menu = GetSystemMenu(hwnd_, false);
            set_menu_item_state(sys_menu, &menu_item_info, SC_RESTORE, isMaximized);
            set_menu_item_state(sys_menu, &menu_item_info, SC_MOVE, !isMaximized);
            set_menu_item_state(sys_menu, &menu_item_info, SC_SIZE, !isMaximized);
            set_menu_item_state(sys_menu, &menu_item_info, SC_MINIMIZE, true);
            set_menu_item_state(sys_menu, &menu_item_info, SC_MAXIMIZE, !isMaximized);
            set_menu_item_state(sys_menu, &menu_item_info, SC_CLOSE, true);
            BOOL const result = TrackPopupMenu(sys_menu, TPM_RETURNCMD, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0, hwnd_, NULL);
            if (result != 0)
            {
                PostMessage(hwnd_, WM_SYSCOMMAND, result, 0);
            }
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);

            // Check active panel for cursor
            Panel *panelCursor = GetPanelManager().GetActivePanel();
            if (panelCursor && panelCursor->IsVisible())
            {
                // Zone de redimensionnement du panel
                if (panelCursor->IsPointInResizeZone(pt))
                {
                    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                    return TRUE;
                }

                // Zone du panel
                if (panelCursor->IsPointInPanel(pt))
                {
                    SetCursor(LoadCursor(NULL, IDC_ARROW));
                    return TRUE;
                }
            }

            // Zone de l'éditeur : curseur texte
            Orion::Editor *editor = GetEditor();
            if (editor)
            {
                RECT tbRect = win32_titlebar_rect(hwnd_);
                RECT clientRect;
                GetClientRect(hwnd_, &clientRect);

                // Si on est en dessous de la barre de titre + tabs, probablement dans l'éditeur
                if (pt.y >= tbRect.bottom + tabBar_.GetHeight())
                {
                    SetCursor(LoadCursor(NULL, IDC_IBEAM));
                    return TRUE;
                }
            }

            // Défaut : flèche
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_DESTROY:
        KillTimer(hwnd_, CARET_TIMER_ID);
        // Shutdown ggwave wrapper
        ggwave::Shutdown();
        CoUninitialize();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, uMsg, wParam, lParam);
}

Window::Window(HINSTANCE hInstance)
    : hInstance_(hInstance), hwnd_(nullptr), skia_(nullptr)
{
    untitledCounter_ = 1;
}

Window::~Window()
{
    for (auto &p : editors_)
    {
        delete p.second;
    }
    editors_.clear();
    if (skia_)
        delete skia_;
}

Orion::Editor *Window::GetEditor()
{
    int idx = tabBar_.GetActiveTabIndex();
    return GetEditorForTab(idx);
}

Orion::Editor *Window::GetEditorForTab(int tabIndex)
{
    auto it = editors_.find(tabIndex);
    if (it != editors_.end())
        return it->second;
    return nullptr;
}

void Window::OpenFileInNewTab(const std::wstring &filePath, int lineNumber)
{
    std::wstring display;
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
        display = filePath.substr(lastSlash + 1);
    else
        display = filePath;

    int tabIndex = tabBar_.AddTab(filePath, display);
    if (editors_.count(tabIndex) == 0)
    {
        Orion::Editor *editor = new Orion::Editor();

        if (!customFontPath_.empty() && skia_)
            editor->LoadCustomFont(skia_->GetDWriteFactory(), customFontPath_);

        if (!filePath.empty())
            editor->LoadFileAsync(hwnd_, filePath, tabIndex);
        else
            editor->CreateEmpty();

        editors_[tabIndex] = editor;
        // Wire document-changed callback so TabBar is updated when editor becomes dirty
        editor->onDocumentChanged = [this, tabIndex]()
        {
            tabBar_.SetTabDirty(tabIndex, true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        };
    }
}

void Window::ClearAllHoverStates()
{
    hoveredButton_ = Hovered_None;
    for (int i = 0; i < 8; ++i)
        SetMenuItemHovered(i, false);
    HideMenuDropdown(hwnd_);

    GetExplorerManager().ClearHover(hwnd_);
    GetPanelManager().ClearResizeHover(hwnd_);
    Footer_ClearHover(hwnd_);

    // tabbar hover se clear ailleurs (WM_MOUSELEAVE / sortie tabbar)
    ThrottledInvalidateRect(hwnd_, nullptr, FALSE);
}

RECT Window::GetTabBarRectClient() const
{
    RECT tbRect = win32_titlebar_rect(hwnd_);
    RECT r;
    r.left   = 0;
    r.top    = tbRect.bottom;
    r.right  = 0;
    r.bottom = tbRect.bottom + (LONG)tabBar_.GetHeight();

    RECT client;
    GetClientRect(hwnd_, &client);

    // Simple option: full client width
    r.right = client.right;

    return r;
}

// Returns true if hover state changed (needs redraw)
bool Window::UpdateTabBarHover(const POINT& ptClient)
{
    RECT r = GetTabBarRectClient();

    const bool inTabBar = (ptClient.x >= r.left && ptClient.x < r.right &&
                           ptClient.y >= r.top  && ptClient.y < r.bottom);

    if (inTabBar)
    {
        int res = tabBar_.OnMouseMove(ptClient);
        if (res != -2) // real change
            return true;
        return false;
    }
    else
    {
        if (tabBar_.ClearHover())
            return true;
        return false;
    }
}
