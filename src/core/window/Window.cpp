#include "core/window/Window.h"
#include "ui/graphics/Skia.h"
#include "../../helpers/path_helpers.h"
#include "utils/logger/Logger.h"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <vector>
#include <cwctype>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <unordered_map>
#include <ctime>
#include <uxtheme.h>
#include <vssym32.h>
#include <commctrl.h>
#include "helpers/window_helpers.h"
#include "ui/components/menu/DropdownMenu.h"
#include "ui/components/titlebar/TitleBar.h"
#include "ui/components/popups/CustomPopup.h"
#include "ui/components/popups/PopupWindow.h"
#include "ui/components/footer/Footer.h"
#include "utils/ggwave/ggwave_integration.h"
#include "core/explorer/Explorer.h"
#include "ui/components/sidebar/Sidebar.h"
#include "ui/components/input/InputTypeFixed.h"
#include "ui/panels/PanelInit.h"
#include "ui/panels/PanelManager.h"
#include "ui/panels/git/GitPanel.h"
#include "ui/panels/git/GitDiffDecorations.h"
#include "ui/panels/search/SearchPanel.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "orion/font/CustomFontLoader.h"
#include "ui/screens/SettingsTab.h"
#include <dwrite_1.h>
#include "ui/panels/ggwave/GGWavePanel.h"
#include "utils/logger/Logger.h"
#include "orion/caret/Caret.h"
#include "ui/layout/ExplorerLayoutState.h"
#include "lsp/LspManager.h"
#include "core/window/OpenFileRequest.h"
#include "utils/update/UpdateService.h"
#include "ui/theme/Theme.h"

static void ApplyDwmWindowStyle(HWND hwnd)
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
    const int DWMSBT_NONE = 1;
    pDwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &DWMSBT_NONE, sizeof(DWMSBT_NONE));

    FreeLibrary(hDwm);
}

static void SetDwmBorderColor(HWND hwnd, bool focused)
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

    // DWMWA_BORDER_COLOR = 34 (Windows 11+). Use COLORREF (0x00bbggrr).
    const DWORD DWMWA_BORDER_COLOR = 34;
    COLORREF color = focused ? RGB(61, 143, 242) : RGB(51, 51, 51);
    pDwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));

    FreeLibrary(hDwm);
}

static const wchar_t *WINDOW_CLASS_NAME = L"NebulaTextWindowClass";
static std::wstring g_tamponText;
static bool g_tamponLoaded = false;

static std::filesystem::path GetTamponStorePath()
{
    PWSTR appDataPath = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath)
    {
        std::filesystem::path base(appDataPath);
        CoTaskMemFree(appDataPath);
        out = base / L"Nebula";
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        out /= L"tampon.txt";
    }
    return out;
}

static std::string WideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
        return {};
    std::string out(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), sizeNeeded, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (sizeNeeded <= 0)
        return {};
    std::wstring out(sizeNeeded, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), sizeNeeded);
    return out;
}

static void LoadTamponFromDisk()
{
    if (g_tamponLoaded)
        return;
    g_tamponLoaded = true;

    std::filesystem::path store = GetTamponStorePath();
    if (store.empty())
        return;

    std::ifstream ifs(store, std::ios::binary);
    if (!ifs)
        return;

    std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
    {
        raw.erase(0, 3);
    }
    g_tamponText = Utf8ToWide(raw);
}

static void SaveTamponToDisk()
{
    std::filesystem::path store = GetTamponStorePath();
    if (store.empty())
        return;
    std::ofstream ofs(store, std::ios::binary | std::ios::trunc);
    if (!ofs)
        return;
    std::string raw = WideToUtf8(g_tamponText);
    if (!raw.empty())
        ofs.write(raw.data(), (std::streamsize)raw.size());
}

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
static constexpr UINT TITLEBAR_HOVER_TIMER_ID = 2;
static constexpr UINT TITLEBAR_HOVER_TIMER_INTERVAL_MS = 16;
static constexpr UINT DIAG_TIMER_ID = 3;
static constexpr UINT DIAG_TIMER_INTERVAL_MS = 80;
static constexpr UINT EDITOR_DRAG_TIMER_ID = 4;
static constexpr UINT EDITOR_DRAG_TIMER_INTERVAL_MS = 16;

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
    auto iconPath = NebulaAssetPath(L"favicon.ico");

    HICON hAppIcon = reinterpret_cast<HICON>(
        LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE | LR_DEFAULTSIZE));

    HICON hAppIconSmall = reinterpret_cast<HICON>(
        LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE | LR_DEFAULTSIZE));

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
    ApplyDwmWindowStyle(hwnd_);
    SetDwmBorderColor(hwnd_, true);

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

static bool IsPointInTitlebarMenuArea(HWND hwnd, POINT pt)
{
    RECT tb = win32_titlebar_rect(hwnd);
    if (pt.y < tb.top || pt.y >= tb.bottom)
        return false;

    CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd, &tb);
    // Ignore area occupied by custom window controls (run/min/max/close)
    if (pt.x >= button_rects.run.left)
        return false;

    // Avoid resize border clicks being treated as menu clicks
    const int border = 4;
    if (pt.x < tb.left + border || pt.x > tb.right - border)
        return false;

    return true;
}

static bool IsPointInMenuSafeZone(POINT pt)
{
    if (!IsMenuDropdownVisible())
        return false;

    D2D1_RECT_F mainR = GetActiveDropdown().rect;
    D2D1_RECT_F subR = IsSubmenuDropdownVisible() ? GetSubmenuDropdown().rect : D2D1::RectF(0, 0, 0, 0);

    auto inRect = [&](const D2D1_RECT_F &r) -> bool
    {
        return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
    };

    if (inRect(mainR))
        return true;
    if (IsSubmenuDropdownVisible() && inRect(subR))
        return true;

    if (IsSubmenuDropdownVisible())
    {
        float left = (std::min)(mainR.right, subR.right);
        float right = (std::max)(mainR.right, subR.left);
        float top = (std::min)(mainR.top, subR.top);
        float bottom = (std::max)(mainR.bottom, subR.bottom);
        const float pad = 10.0f;
        D2D1_RECT_F corridor = D2D1::RectF(left - pad, top - pad, right + pad, bottom + pad);
        if (inRect(corridor))
            return true;
    }

    return false;
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

const std::wstring &GetTamponText()
{
    LoadTamponFromDisk();
    return g_tamponText;
}

bool HasTamponText()
{
    LoadTamponFromDisk();
    return !g_tamponText.empty();
}

void SetTamponText(const std::wstring &text)
{
    LoadTamponFromDisk();
    g_tamponText = text;
    SaveTamponToDisk();
}

void ClearTamponText()
{
    LoadTamponFromDisk();
    g_tamponText.clear();
    SaveTamponToDisk();
}

HWND Window::GetHwnd() const
{
    return hwnd_;
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

        UINT dpi = win32_get_dpi_for_window(hwnd_);
        int frame_x = win32_get_system_metrics_for_dpi(SM_CXFRAME, dpi);
        int frame_y = win32_get_system_metrics_for_dpi(SM_CYFRAME, dpi);
        int padding = win32_get_system_metrics_for_dpi(SM_CXPADDEDBORDER, dpi);

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
        UI::Theme::Initialize();
        UI::Theme::SetWindowFocused(true);
        skia_ = new Skia();
        if (!skia_->Init(hwnd_))
        {
            delete skia_;
            skia_ = nullptr;
            return -1;
        }
        GetExplorerManager().PreloadIconMapAsync();
        // Load custom font for all future editors
        {
            wchar_t modulePath[MAX_PATH] = {0};
            if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0)
            {
                std::wstring dir(modulePath);
                size_t pos = dir.find_last_of(L"\\/");
                if (pos != std::wstring::npos)
                    dir = dir.substr(0, pos);

                auto fontPath = NebulaAssetPath(L"font\\static\\JetBrainsMono-Regular.ttf");

                if (std::filesystem::exists(fontPath))
                {
                    Logger::Instance().Log(L"[Window] Custom font path: " + fontPath.wstring());
                    customFontPath_ = fontPath.wstring();
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
                            uiFontCollection_ = fontCollection;
                            uiFontCollection_->AddRef();
                            GetTerminalPanel().SetFont(L"JetBrains Mono", 13.0f);
                            fontCollection->AddRef();
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
        SetText(L"Bonjour — texte rendu via GPU (Direct2D)");

        SetTimer(hwnd_, CARET_TIMER_ID, CARET_TIMER_INTERVAL_MS, nullptr);
        SetTimer(hwnd_, EDITOR_DRAG_TIMER_ID, EDITOR_DRAG_TIMER_INTERVAL_MS, nullptr);
        DragAcceptFiles(hwnd_, TRUE);

        // Initialize ggwave wrapper (will fallback to SAPI if not enabled)
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        ggwave::Initialize();

        // Initialize the panel system
        InitializePanelSystem();
        // Initialize keyboard manager when HWND is available
        keyboard_.Init(this);
        UpdateService::EnsureInitialized();
        lastUpdateStateSnapshot_ = static_cast<int>(UpdateService::GetState());
        lastUpdateStatusMessage_ = UpdateService::GetStatusMessage();
        bool openedFromArgs = HandleCommandLineArgs();

        // If no project yet and nothing opened from args, prompt for project creation
        if (!openedFromArgs && GetExplorerManager().GetState().rootPath.empty())
            PostMessageW(hwnd_, WM_OPEN_NEW_PROJECT, 0, 0);

        RECT clientRect;
        GetClientRect(hwnd_, &clientRect);
        UINT dpiInit = win32_get_dpi_for_window(hwnd_);
        UINT initW = MulDiv(clientRect.right - clientRect.left, dpiInit, 96);
        UINT initH = MulDiv(clientRect.bottom - clientRect.top, dpiInit, 96);
        skia_->Resize(initW, initH);

        SetWindowPos(hwnd_, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_ACTIVATE:
    {
        bool focused = (LOWORD(wParam) != WA_INACTIVE);
        UI::Theme::SetWindowFocused(focused);
        SetDwmBorderColor(hwnd_, focused);
        RECT title_bar_rect = win32_titlebar_rect(hwnd_);
        // Clear hovered state when window activation changes
        hoveredButton_ = Hovered_None;
        for (size_t i = 0; i < GetMenuItems().size(); ++i)
            SetMenuItemHovered((int)i, false);
        HideMenuDropdown(hwnd_);
        InvalidateRect(hwnd_, &title_bar_rect, FALSE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_OPEN_NEW_PROJECT:
    {
        if (skipNewProjectOverlayOnce_)
        {
            skipNewProjectOverlayOnce_ = false;
            return 0;
        }
        ShowNewProjectOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_SHOW_RUN_ERROR_POPUP:
    {
        std::wstring *title = reinterpret_cast<std::wstring *>(lParam);
        if (title)
        {
            RECT rc;
            GetWindowRect(hwnd_, &rc);
            UINT dpi = win32_get_dpi_for_window(hwnd_);
            int width = win32_dpi_scale(520, dpi);
            int height = win32_dpi_scale(220, dpi);
            int x = rc.left + (rc.right - rc.left - width) / 2;
            int y = rc.top + (rc.bottom - rc.top - height) / 3;
            size_t split = title->find(L'\n');
            std::wstring msgTitle = *title;
            std::wstring msgBody;
            if (split != std::wstring::npos)
            {
                msgTitle = title->substr(0, split);
                msgBody = title->substr(split + 1);
            }
            ShowPopupWindow(hwnd_, x, y, width, height, msgTitle, msgBody);
            delete title;
        }
        return 0;
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
            if (newProjectVisible_)
                return HTCLIENT;
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
            UINT dpi = win32_get_dpi_for_window(hwnd_);
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
        TRACKMOUSEEVENT tmeNc = {};
        tmeNc.cbSize = sizeof(tmeNc);
        tmeNc.dwFlags = TME_LEAVE | TME_NONCLIENT;
        tmeNc.hwndTrack = hwnd_;
        TrackMouseEvent(&tmeNc);

        if (newProjectVisible_)
        {
            // Only allow system buttons hover when the overlay is visible.
            CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd_, &title_bar_rect);
            Window::CustomTitleBarHoveredButton new_hovered_button = Window::Hovered_None;
            if (PtInRect(&button_rects.close, cursor_point))
                new_hovered_button = Window::Hovered_Close;
            else if (PtInRect(&button_rects.minimize, cursor_point))
                new_hovered_button = Window::Hovered_Minimize;
            else if (PtInRect(&button_rects.maximize, cursor_point))
                new_hovered_button = Window::Hovered_Maximize;

            if (new_hovered_button != hoveredButton_)
            {
                InvalidateRect(hwnd_, &button_rects.close, FALSE);
                InvalidateRect(hwnd_, &button_rects.minimize, FALSE);
                InvalidateRect(hwnd_, &button_rects.maximize, FALSE);
                hoveredButton_ = new_hovered_button;
                StartTitlebarHoverAnimation();
            }
            return DefWindowProc(hwnd_, uMsg, wParam, lParam);
        }

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
            StartTitlebarHoverAnimation();
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_NCMOUSELEAVE:
    {
        if (hoveredButton_ != Hovered_None)
        {
            hoveredButton_ = Hovered_None;
            StartTitlebarHoverAnimation();
            RECT tb = win32_titlebar_rect(hwnd_);
            InvalidateRect(hwnd_, &tb, FALSE);
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_EDITOR_FILE_LOADED:
    {
        auto *res = reinterpret_cast<EditorFileLoadResult *>(lParam);
        if (!res)
            return 0;


        Orion::Editor *ed = GetEditorForTab(res->tabIndex);
        bool applied = false;
        if (ed)
        {
            // IMPORTANT: v??rifier que l?????diteur correspond toujours au m??me fichier (tab peut avoir chang??)
            if (ed->GetFilePath() == res->filePath)
            {
                if (res->isPreview)
                {
                    ed->ApplyLoadedPreview(
                        std::move(res->filePath),
                        res->previewBitmap,
                        res->previewSize,
                        std::move(res->previewMessage));
                }
                else
                {
                    std::wstring loadedPath = res->filePath;
                    ed->ApplyLoadedFile(std::move(res->filePath), std::move(res->encoding), std::move(res->lines));

                    if (GitDiffDecorations::ConsumePendingSplitOpen(loadedPath))
                    {
                        GitDiffDecorations::SplitViewData splitData;
                        if (GitDiffDecorations::GetSplitForFile(loadedPath, splitData))
                        {
                            std::vector<Orion::Editor::GitSplitDiffRow> rows;
                            rows.reserve(splitData.rows.size());
                            for (const auto &row : splitData.rows)
                            {
                                Orion::Editor::GitSplitDiffRow outRow;
                                outRow.leftText = row.leftText;
                                outRow.rightText = row.rightText;
                                outRow.hasLeft = row.hasLeft;
                                outRow.hasRight = row.hasRight;
                                outRow.leftDeleted = row.leftDeleted;
                                outRow.rightAdded = row.rightAdded;
                                rows.push_back(std::move(outRow));
                            }
                            ed->SetGitSplitDiffView(rows);
                        }
                    }

                    auto it = pendingGoToLocation_.find(res->tabIndex);
                    if (it != pendingGoToLocation_.end())
                    {
                        int targetLine = it->second.first;
                        int targetCol = it->second.second;
                        pendingGoToLocation_.erase(it);
                        if (targetLine < 0)
                            targetLine = 0;
                        if (targetCol < 0)
                            targetCol = 0;
                        Orion::Caret::SetCaret(*ed, targetLine, targetCol);
                        ed->RevealCaretOnNextLayout();
                    }

                    Lsp::LspManager::Instance().UpdateFile(ed->GetFilePath(), ed->GetLinesSnapshot());
                    Lsp::LspManager::Instance().RequestDiagnosticsAsync(ed->GetFilePath(), ed->GetLinesSnapshot(), hwnd_, res->tabIndex);
                }
                if (pendingMarkdownPreview_.find(res->tabIndex) != pendingMarkdownPreview_.end())
                {
                    ed->SetMarkdownPreviewEnabled(true);
                    tabBar_.SetTabMarkdownPreview(res->tabIndex, true);
                    pendingMarkdownPreview_.erase(res->tabIndex);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                applied = true;
            }
        }

        if (!applied && res->isPreview && res->previewBitmap)
            DeleteObject(res->previewBitmap);

        delete res;
        return 0;
    }

    case WM_USER + 100:
    {
        auto *pPath = reinterpret_cast<std::wstring *>(lParam);
        int lineNumber = (int)wParam;

        if (pPath)
        {
            OpenFileInNewTab(*pPath, lineNumber >= 0 ? lineNumber : -1, -1);
            delete pPath;
        }
        return 0;
    }
    case WM_OPEN_FILE_AT:
    {
        auto *req = reinterpret_cast<OpenFileRequest *>(lParam);
        if (req)
        {
            OpenFileInNewTab(req->filePath, req->line, req->column);
            delete req;
        }
        return 0;
    }

    case WM_LSP_DIAGNOSTICS:
    {
        auto *payload = reinterpret_cast<LspDiagnosticsResult *>(lParam);
        if (payload)
        {
            Orion::Editor *ed = GetEditorForTab(payload->tabIndex);
            if (ed && ed->GetFilePath() == payload->filePath)
            {
                ed->SetDiagnostics(payload->diagnostics);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            delete payload;
        }
        return 0;
    }

    case WM_EDITOR_FILE_RENAMED:
    {
        auto *payload = reinterpret_cast<RenamePathPayload *>(lParam);
        if (payload)
        {
            int tabIndex = tabBar_.FindTabIndexByFilePath(payload->oldPath);
            if (tabIndex >= 0)
            {
                std::wstring display;
                size_t lastSlash = payload->newPath.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos)
                    display = payload->newPath.substr(lastSlash + 1);
                else
                    display = payload->newPath;

                tabBar_.UpdateTabPath(tabIndex, payload->newPath, display);
                Orion::Editor *ed = GetEditorForTab(tabIndex);
                if (ed)
                {
                    ed->SetFilePath(payload->newPath);
                }

                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            delete payload;
        }
        return 0;
    }

    case WM_OPEN_SETTINGS:
    {
        OpenSettingsTab();
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
    case WM_DROPFILES:
    {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        if (!hDrop)
            return 0;

        POINT pt = {0, 0};
        BOOL inClient = DragQueryPoint(hDrop, &pt);

        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> droppedPaths;
        droppedPaths.reserve(fileCount);
        for (UINT i = 0; i < fileCount; ++i)
        {
            UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
            if (pathLen == 0)
                continue;

            std::wstring path;
            path.resize(pathLen + 1);
            UINT written = DragQueryFileW(hDrop, i, path.data(), pathLen + 1);
            if (written == 0)
                continue;

            path.resize(written);
            droppedPaths.push_back(std::move(path));
        }
        DragFinish(hDrop);

        if (!inClient || droppedPaths.empty())
            return 0;

        ExplorerManager &explorer = GetExplorerManager();
        bool handled = false;
        bool droppedInExplorer = explorer.IsVisible() && explorer.IsPointInExplorer(pt);

        if (droppedInExplorer && !explorer.GetState().rootPath.empty())
            handled = explorer.HandleExternalDrop(hwnd_, pt, droppedPaths);

        if (!handled)
        {
            for (const std::wstring &path : droppedPaths)
            {
                std::error_code ec;
                if (std::filesystem::is_directory(path, ec))
                {
                    OpenProjectAtPath(path);
                    handled = true;
                    break;
                }
            }
        }

        if (!handled)
        {
            for (const std::wstring &path : droppedPaths)
            {
                std::error_code ec;
                if (std::filesystem::is_regular_file(path, ec))
                {
                    OpenFileInNewTab(path, -1, -1);
                    handled = true;
                }
            }
        }

        if (handled)
            InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    
    case WM_LBUTTONDOWN:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        if (newProjectVisible_)
        {
            if (HandleNewProjectMouseDown(hwnd_, pt))
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        // Popup interaction (click-through popup)
        if (GetActivePopupWindow())
        {
            POINT screenPt = pt;
            ClientToScreen(hwnd_, &screenPt);
            if (PopupHitTestClose(screenPt))
            {
                CloseActivePopupWindow();
                return 0;
            }
            if (PopupHitTestHeader(screenPt))
            {
                PopupStartDrag(screenPt);
                SetCapture(hwnd_);
                return 0;
            }
        }

        // If a titlebar menu dropdown is visible, let it handle clicks first
        if (IsMenuDropdownVisible())
        {
            if (IsSubmenuDropdownVisible() && IsPointInSubmenu(pt))
            {
                int subIndex = GetSubmenuHoveredItem(pt);
                if (subIndex >= 0)
                {
                    int baseId = GetSubmenuDropdown().baseId;
                    int commandId = baseId + subIndex;
                    PostMessageW(hwnd_, WM_COMMAND, commandId, 0);
                    HideSubmenuDropdown(hwnd_);
                    HideMenuDropdown(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }

            int itemIndex = GetDropdownHoveredItem(pt);
            if (itemIndex >= 0)
            {
                int baseId = GetActiveDropdown().baseId;
                if (baseId >= 5000 && baseId < 6000)
                {
                    MenuDropdown &dd = GetActiveDropdown();
                    bool hasSub = (!dd.hasSubmenu.empty() && itemIndex < (int)dd.hasSubmenu.size() && dd.hasSubmenu[itemIndex]);
                    if (hasSub)
                    {
                        std::vector<std::wstring> items = {
                            L"Nouveau fichier",
                            L"Nouveau dossier",
                            L"Class Header (.h)",
                            L"Class Source (.cpp)"};
                        D2D1_RECT_F r = dd.rect;
                        float itemHeight = (r.bottom - r.top) / (dd.items.empty() ? 1.0f : (float)dd.items.size());
                        D2D1_RECT_F itemRect = D2D1::RectF(r.left, r.top + itemIndex * itemHeight, r.right, r.top + (itemIndex + 1) * itemHeight);
                        ShowSubmenuDropdown(hwnd_, items, D2D1::Point2F(itemRect.right - 1.0f, itemRect.top), 9000);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    int commandId = baseId + itemIndex;
                    GetExplorerManager().HandleContextCommand(commandId);
                }
                else if (baseId >= 7000 && baseId < 9000)
                {
                    int commandId = baseId + itemIndex;
                    PostMessageW(hwnd_, WM_COMMAND, commandId, 0);
                }
                else
                {
                    int menuIndex = GetActiveDropdown().menuIndex;
                    if (menuIndex == 0 && itemIndex == 3)
                    {
                        // Open Recent submenu
                        LoadRecentProjects();
                        D2D1_RECT_F r = GetActiveDropdown().rect;
                        MenuDropdown &dd = GetActiveDropdown();
                        float itemHeight = (r.bottom - r.top) / (dd.items.empty() ? 1.0f : (float)dd.items.size());
                        D2D1_RECT_F itemRect = D2D1::RectF(r.left, r.top + itemIndex * itemHeight, r.right, r.top + (itemIndex + 1) * itemHeight);

                        std::vector<std::wstring> items;
                        for (const auto &p : recentProjects_)
                        {
                            std::filesystem::path fp(p.path);
                            std::wstring name = fp.filename().wstring();
                            if (name.empty())
                                name = p.path;
                            items.push_back(name + L"  \u2014  " + p.path);
                        }
                        if (items.empty())
                            items.push_back(L"(Aucun recent)");

                        // Keep submenu flush with main menu to avoid mouse gap.
                        ShowSubmenuDropdown(hwnd_, items, D2D1::Point2F(itemRect.right - 1.0f, itemRect.top), 8000);
                        if (items.size() == 1 && recentProjects_.empty())
                        {
                            MenuDropdown &sd = GetSubmenuDropdown();
                            sd.enabled.clear();
                            sd.enabled.resize(items.size(), false);
                        }
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    int commandId = 3000 + menuIndex * 100 + itemIndex;
                    PostMessageW(hwnd_, WM_COMMAND, commandId, 0);
                }
                HideSubmenuDropdown(hwnd_);
                HideMenuDropdown(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (!IsPointInMenuSafeZone(pt))
            {
                HideSubmenuDropdown(hwnd_);
                HideMenuDropdown(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            else
            {
                return 0;
            }
        }

        if (IsPointInUpdateToast(pt) && UpdateService::HasUpdateAvailable())
        {
            std::wstring err;
            if (UpdateService::InstallUpdateAndRestart(err))
            {
                Footer_SetHint(hwnd_, L"Mise a jour en cours... redemarrage automatique", 2600);
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            }
            else
                Footer_SetHint(hwnd_, err.empty() ? L"Impossible d'ouvrir le setup." : err, 2600);
            return 0;
        }

        // Global Input overlay removed; clicks always propagate to Explorer/editor.

        // Prioritize panel resize zone over explorer hit (prevents dead resize area)
        Panel *activePanelClick = GetPanelManager().GetActivePanel();
        if (activePanelClick && activePanelClick->IsVisible() && activePanelClick->IsPointInResizeZone(pt))
        {
            GetPanelManager().OnLeftButtonDown(hwnd_, pt);
            return 0;
        }

        TerminalPanel &terminal = GetTerminalPanel();

        // Explorer click has priority over terminal routing.
        if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
        {
            // If terminal still owns capture from a previous interaction, release it first
            // so explorer drag/scroll receives a clean mouse sequence.
            if (terminal.IsVisible() && terminal.HasMouseCapture() && GetCapture() == hwnd_)
            {
                terminal.OnLeftButtonUp(hwnd_);
            }
            GetExplorerManager().OnLeftButtonDown(hwnd_, pt);
            return 0;
        }

        // Check if click is in terminal area (tabs/plus/resize/panel)
        bool hitTerminal = false;
        if (terminal.IsVisible())
        {
            bool inTerminal = terminal.IsPointInPanel(pt);
            bool inTerminalResize = terminal.IsPointInResizeZone(pt);
            bool inTerminalTabs = terminal.IsPointInTabsBarArea(pt) || terminal.IsPointInPlusButton(pt);

            if (inTerminal || inTerminalResize || inTerminalTabs)
            {
                hitTerminal = true;
                ReleaseCapture();
                Orion::Editor *editor = GetEditor();
                if (editor)
                    editor->CancelInteraction();

                terminal.OnLeftButtonDown(hwnd_, pt);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
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
                if (r == TabBar::TAB_CLICKED_TOGGLE_PREVIEW)
                {
                    int idx = tabBar_.GetLastPreviewToggleIndex();
                    if (idx >= 0)
                    {
                        Orion::Editor *ed = GetEditorForTab(idx);
                        if (ed)
                        {
                            bool enable = !ed->IsMarkdownPreviewEnabled();
                            ed->SetMarkdownPreviewEnabled(enable);
                            tabBar_.SetTabMarkdownPreview(idx, enable);
                        }
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                }
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

        int hoveredMenu = -1;
        if (IsPointInTitlebarMenuArea(hwnd_, pt))
            hoveredMenu = GetHoveredMenuItem(hwnd_, pt);
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
            HideSubmenuDropdown(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // terminal handled above (before explorer)

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

            // Click outside terminal - unfocus it
            if (!hitTerminal)
                terminal.Unfocus();

            GetPanelManager().OnLeftButtonDown(hwnd_, pt);
            return 0;
        }

        // Check if click is on GGWave button
        GGWavePanel &ggwave = GetGGWavePanel();
        if (ggwave.IsPointOnButton(pt))
        {
            ggwave.OnLeftButtonDown(hwnd_, pt);
            if (!hitTerminal)
                terminal.Unfocus();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (terminal.IsVisible() && !hitTerminal)
            terminal.Unfocus();

        if (IsSettingsTabActive() && settingsTab_ && settingsTab_->IsPointInView(pt))
        {
            if (GetPanelManager().IsPanelActive(PanelId::Search))
            {
                SearchPanel *searchPanel = GetPanelManager().GetPanelAs<SearchPanel>(PanelId::Search);
                if (searchPanel && searchPanel->IsInputFocused())
                {
                    searchPanel->UnfocusInput();
                }
            }
            if (GetPanelManager().IsPanelActive(PanelId::Git))
            {
                GitPanel *gitPanel = GetPanelManager().GetPanelAs<GitPanel>(PanelId::Git);
                if (gitPanel && gitPanel->IsInputFocused())
                    gitPanel->UnfocusInputs();
            }

            GetTerminalPanel().Unfocus();
            settingsTab_->OnLeftButtonDown(hwnd_, pt);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        Orion::Editor *editor = GetEditor();
        if (editor && editor->IsPointInEditorBounds(pt))
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
            if (GetPanelManager().IsPanelActive(PanelId::Git))
            {
                GitPanel *gitPanel = GetPanelManager().GetPanelAs<GitPanel>(PanelId::Git);
                if (gitPanel && gitPanel->IsInputFocused())
                    gitPanel->UnfocusInputs();
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
        UINT dpi = win32_get_dpi_for_window(hwnd_);
        int sidebarWidth = win32_dpi_scale(50, dpi);

        // Get panel width from active panel
        Panel *panelDblClick = GetPanelManager().GetActivePanel();
        int panelLeftWidth = 0;
        int panelRightWidth = 0;
        if (panelDblClick && panelDblClick->IsVisible())
        {
            int activeWidth = panelDblClick->GetPhysicalWidth();
            if (panelDblClick->GetId() == PanelId::Explorer &&
                GetExplorerLayoutState().placement == ExplorerPlacement::Right)
            {
                panelRightWidth = activeWidth;
            }
            else
            {
                panelLeftWidth = activeWidth;
            }
        }
        int editorLeftX = sidebarWidth + panelLeftWidth;
        int editorRightX = clientRect.right - panelRightWidth;

        bool inPanelDbl = panelDblClick && panelDblClick->IsVisible() &&
                          (panelDblClick->IsPointInPanel(pt) || panelDblClick->IsPointInResizeZone(pt));

        if (tabBar_.GetActiveTabIndex() < 0 &&
            pt.y >= tbRect.bottom + tabBar_.GetHeight() && pt.y <= clientRect.bottom &&
            pt.x >= editorLeftX && pt.x <= editorRightX &&
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
                newEditor->onDocumentChanged = [this, tabIndex]()
                {
                    tabBar_.SetTabDirty(tabIndex, true);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                };

                if (HasTamponText())
                    newEditor->SetTextContent(GetTamponText(), true);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        Orion::Editor *editor = GetEditor();
        if (editor && editor->IsPointInEditorBounds(pt))
        {
            if (GetPanelManager().IsPanelActive(PanelId::Search))
            {
                SearchPanel *searchPanel = GetPanelManager().GetPanelAs<SearchPanel>(PanelId::Search);
                if (searchPanel && searchPanel->IsInputFocused())
                    searchPanel->UnfocusInput();
            }
            if (GetPanelManager().IsPanelActive(PanelId::Git))
            {
                GitPanel *gitPanel = GetPanelManager().GetPanelAs<GitPanel>(PanelId::Git);
                if (gitPanel && gitPanel->IsInputFocused())
                    gitPanel->UnfocusInputs();
            }

            GetTerminalPanel().Unfocus();
            editor->OnLeftButtonDown(hwnd_, pt);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    case WM_CHAR:
    {
        if (newProjectVisible_)
        {
            HandleNewProjectChar(static_cast<wchar_t>(wParam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (IsSettingsTabActive() && settingsTab_)
        {
            if (settingsTab_->OnChar(static_cast<wchar_t>(wParam)))
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (keyboard_.OnChar(wParam))
            return 0;

        return 0;
    }
    case WM_KEYDOWN:
    {
        if (wParam == VK_ESCAPE && CloseActivePopupWindow())
            return 0;
        if (newProjectVisible_)
        {
            HandleNewProjectKeyDown(wParam);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        // ✅ Toute la logique est dans KeyboardManager
        if (IsSettingsTabActive() && settingsTab_)
        {
            if (settingsTab_->OnKeyDown(wParam))
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (keyboard_.OnKeyDown(wParam))
            return 0;

        // fallback
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &pt);

        if (newProjectVisible_)
            return 0;

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
            // Don't forward wheel to editor when Output/Problems is shown.
            if (terminalWheel.IsShowingOutputOrProblems())
            {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                terminalWheel.OnMouseWheel(hwnd_, delta);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            terminalWheel.OnMouseWheel(hwnd_, delta);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (IsSettingsTabActive() && settingsTab_ && settingsTab_->IsPointInView(pt))
        {
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

        if (newProjectVisible_)
            return 0;

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

        if (newProjectVisible_)
        {
            HandleNewProjectMouseMove(hwnd_, pt);
            ThrottledInvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (GetActivePopupWindow())
        {
            POINT screenPt = pt;
            ClientToScreen(hwnd_, &screenPt);
            PopupSetHoverClose(PopupHitTestClose(screenPt));
            if (PopupIsDragging() && GetCapture() == hwnd_)
            {
                PopupDragTo(screenPt);
                return 0;
            }
        }

        // If Explorer scrollbar is dragging, keep routing moves even outside its bounds.
        if (GetExplorerManager().IsVisible() && GetExplorerManager().IsScrollbarDragging())
        {
            GetExplorerManager().OnMouseMove(hwnd_, pt);
            return 0;
        }

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

        if (SetUpdateToastHovered(IsPointInUpdateToast(pt)))
            ThrottledInvalidateRect(hwnd_, nullptr, FALSE);

        // If we're inside the tabbar -> consume the event here
        RECT tabRect = GetTabBarRectClient();
        bool inTabBar = (pt.x >= tabRect.left && pt.x < tabRect.right &&
                         pt.y >= tabRect.top  && pt.y < tabRect.bottom);
        if (inTabBar)
        {
            // Optional: avoid explorer hover while over tabbar
            GetExplorerManager().ClearHover(hwnd_);
            if (hoveredButton_ != Hovered_None)
            {
                hoveredButton_ = Hovered_None;
                StartTitlebarHoverAnimation();
                RECT tb = win32_titlebar_rect(hwnd_);
                InvalidateRect(hwnd_, &tb, FALSE);
            }
            return 0;
        }

        // ============================================================
        // Rest: normal routing
        // ============================================================
        bool needsRedraw = false;
        bool pointInExplorer = GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt);

        // Check if active panel is resizing
        Panel* activePanel = GetPanelManager().GetActivePanel();
        bool panelResizing = activePanel && activePanel->IsResizing();
        if (panelResizing)
        {
            GetPanelManager().OnMouseMove(hwnd_, pt);
            return 0;
        }

        bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (hoveredButton_ != Hovered_None)
        {
            RECT tb = win32_titlebar_rect(hwnd_);
            if (pt.y >= tb.bottom)
            {
                hoveredButton_ = Hovered_None;
                StartTitlebarHoverAnimation();
                InvalidateRect(hwnd_, &tb, FALSE);
            }
        }

        // ===== TERMINAL HANDLING - Optimized =====
        TerminalPanel& terminal = GetTerminalPanel();
        bool terminalHasCapture = terminal.HasMouseCapture() && (GetCapture() == hwnd_);
        bool inTerminalArea = terminal.IsVisible() &&
            !pointInExplorer &&
            (terminal.IsPointInPanel(pt) || terminal.IsPointInResizeZone(pt) ||
              terminal.IsPointInTabsBarArea(pt) || terminal.IsPointInPlusButton(pt));
        bool terminalCaptureDrag = terminalHasCapture && lmbDown && !pointInExplorer;
        if (terminal.IsVisible() && (inTerminalArea || terminalCaptureDrag))
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

        // Keep terminal hover state in sync while pointer is over terminal area.
        if (terminal.IsVisible() && !lmbDown && inTerminalArea)
        {
            bool changed = terminal.OnMouseMove(hwnd_, pt);
            if (changed)
            {
                const auto& st = terminal.GetState();
                RECT tr = {(LONG)st.leftEdge, (LONG)st.topEdge, (LONG)st.rightEdge, (LONG)st.bottomEdge};
                ThrottledInvalidateRect(hwnd_, &tr, FALSE);
            }
        }

        // When terminal owns the pointer region (or capture), stop routing hover
        // to unrelated UI (editor/explorer/panels) to avoid hover flicker/state conflicts.
        if (terminal.IsVisible() && (inTerminalArea || terminalCaptureDrag))
            return 0;

        // GGWave button hover
        GetGGWavePanel().OnMouseMove(hwnd_, pt);

        // If left button is down, prioritize editor dragging selection
        if (lmbDown)
        {
            if (IsSettingsTabActive() && settingsTab_)
            {
                settingsTab_->OnMouseMove(hwnd_, pt);
                return 0;
            }
            Orion::Editor* editor = GetEditor();
            if (editor && (GetCapture() == hwnd_ || editor->IsPointInEditorBounds(pt)))
            {
                editor->OnMouseMove(hwnd_, pt);
                ThrottledInvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }

        // Menu dropdown handling
        if (IsMenuDropdownVisible())
        {
            if (IsSubmenuDropdownVisible() && IsPointInSubmenu(pt))
            {
                int subHovered = GetSubmenuHoveredItem(pt);
                if (subHovered != GetSubmenuDropdown().hoveredItem)
                {
                    SetSubmenuHoveredItem(subHovered);
                    needsRedraw = true;
                }
                if (needsRedraw)
                    ThrottledInvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            int hoveredItem = GetDropdownHoveredItem(pt);
            if (hoveredItem != GetActiveDropdown().hoveredItem)
            {
                SetDropdownHoveredItem(hoveredItem);
                needsRedraw = true;
            }

            int baseId = GetActiveDropdown().baseId;
            if (baseId >= 5000 && baseId < 6000)
            {
                MenuDropdown &dd = GetActiveDropdown();
                bool hasSub = (hoveredItem >= 0 && !dd.hasSubmenu.empty() && hoveredItem < (int)dd.hasSubmenu.size() && dd.hasSubmenu[hoveredItem]);
                if (hasSub)
                {
                    std::vector<std::wstring> items = {
                        L"Nouveau fichier",
                        L"Nouveau dossier",
                        L"Class Header (.h)",
                        L"Class Source (.cpp)"};
                    D2D1_RECT_F r = dd.rect;
                    float itemHeight = (r.bottom - r.top) / (dd.items.empty() ? 1.0f : (float)dd.items.size());
                    D2D1_RECT_F itemRect = D2D1::RectF(r.left, r.top + hoveredItem * itemHeight, r.right, r.top + (hoveredItem + 1) * itemHeight);
                    ShowSubmenuDropdown(hwnd_, items, D2D1::Point2F(itemRect.right - 1.0f, itemRect.top), 9000);
                }
                else if (IsSubmenuDropdownVisible() && !IsPointInSubmenu(pt))
                {
                    // Only hide if mouse is NOT in submenu
                    HideSubmenuDropdown(hwnd_);
                }
            }

            int originMenu = GetActiveDropdown().menuIndex;
            for (size_t i = 0; i < GetMenuItems().size(); ++i)
                SetMenuItemHovered((int)i, (int)i == originMenu);

            if (originMenu == 0 && hoveredItem == 3)
            {
                LoadRecentProjects();
                std::vector<std::wstring> items;
                for (const auto &p : recentProjects_)
                {
                    std::filesystem::path fp(p.path);
                    std::wstring name = fp.filename().wstring();
                    if (name.empty())
                        name = p.path;
                    items.push_back(name + L"  \u2014  " + p.path);
                }
                if (items.empty())
                    items.push_back(L"(Aucun recent)");

                D2D1_RECT_F r = GetActiveDropdown().rect;
                float itemHeight = (r.bottom - r.top) / (GetActiveDropdown().items.empty() ? 1.0f : (float)GetActiveDropdown().items.size());
                D2D1_RECT_F itemRect = D2D1::RectF(r.left, r.top + hoveredItem * itemHeight, r.right, r.top + (hoveredItem + 1) * itemHeight);
                // Keep submenu flush with main menu to avoid mouse gap.
                ShowSubmenuDropdown(hwnd_, items, D2D1::Point2F(itemRect.right - 1.0f, itemRect.top), 8000);
                if (items.size() == 1 && recentProjects_.empty())
                {
                    MenuDropdown &dd = GetSubmenuDropdown();
                    dd.enabled.clear();
                    dd.enabled.resize(items.size(), false);
                }
            }
            else if (originMenu >= 0 && IsSubmenuDropdownVisible())
            {
                HideSubmenuDropdown(hwnd_);
            }

            if (needsRedraw)
                ThrottledInvalidateRect(hwnd_, nullptr, FALSE);

            return 0;
        }

        // Title bar menu hover
        RECT title_bar_rect = win32_titlebar_rect(hwnd_);
        if (pt.y < title_bar_rect.bottom && IsPointInTitlebarMenuArea(hwnd_, pt))
        {
            GetPanelManager().ClearResizeHover(hwnd_);

            int hoveredMenu = GetHoveredMenuItem(hwnd_, pt);
            static int lastHoveredMenu = -1;
            if (hoveredMenu != lastHoveredMenu)
            {
                for (size_t i = 0; i < GetMenuItems().size(); ++i)
                    SetMenuItemHovered((int)i, (int)i == hoveredMenu);
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
                UINT dpi = win32_get_dpi_for_window(hwnd_);
                int footerLogicalH = 28;
                int footerH = win32_dpi_scale(footerLogicalH, dpi);

                if (pt.y >= client.bottom - footerH)
                {
                    Footer_OnMouseMove(hwnd_, pt);
                    return 0;
                }

                // Explorer hover (ignore terminal panel area)
                TerminalPanel &terminalHoverGuard = GetTerminalPanel();
                bool inExplorer = GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt);
                bool inTerminalPanel = !inExplorer && terminalHoverGuard.IsVisible() && terminalHoverGuard.IsPointInPanel(pt);

                if (!inExplorer)
                    ClearAllHoverStates();

                if (inExplorer)
                {
                    GetExplorerManager().OnMouseMove(hwnd_, pt);
                }
                else
                {
                    if (inTerminalPanel)
                    {
                        GetExplorerManager().ClearHover(hwnd_);
                    }

                    if (IsSettingsTabActive() && settingsTab_)
                    {
                        settingsTab_->OnMouseMove(hwnd_, pt);
                    }
                    else
                    {
                        Orion::Editor* editor = GetEditor();
                        if (editor)
                            editor->OnMouseMove(hwnd_, pt);
                    }
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

        if (newProjectVisible_)
        {
            HandleNewProjectMouseUp(hwnd_, pt);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (PopupIsDragging() && GetCapture() == hwnd_)
        {
            PopupEndDrag();
            ReleaseCapture();
            return 0;
        }

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
        if (terminalUp.IsVisible() && terminalUp.HasMouseCapture() && GetCapture() == hwnd_)
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
                popupItems = {L"Undo", L"Cut", L"Copy", L"Paste", L"Delete", L"Select All"};
                break;
            case 2:
                popupItems = {L"Definir Tampon...", L"Effacer Tampon", L"Voir Tampon"};
                break;
            case 3:
                popupItems = {L"Select All", L"Expand Selection", L"Shrink Selection", L"Select Line"};
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
        if (GetExplorerManager().IsVisible() &&
            (GetExplorerManager().IsPointInExplorer(pt) || GetExplorerManager().IsScrollbarDragging()))
        {
            GetExplorerManager().OnLeftButtonUp(hwnd_);
            ReleaseCapture();
            return 0;
        }
        else
        {
            if (IsSettingsTabActive() && settingsTab_)
            {
                settingsTab_->OnLeftButtonUp(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

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
        if (GetActivePopupWindow())
            PopupSetHoverClose(false);

        if (newProjectVisible_)
        {
            newProjBrowseHover_ = false;
            newProjOpenHover_ = false;
            newProjCancelHover_ = false;
            newProjCreateHover_ = false;
            newProjTemplateHover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        // ✅ guarantees tabbar hover is clean
        tabBar_.ClearHover();
        RECT tabRect = GetTabBarRectClient();
        InvalidateRect(hwnd_, &tabRect, FALSE);

        if (settingsTab_)
        {
            POINT off = {-1, -1};
            settingsTab_->OnMouseMove(hwnd_, off);
        }

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

            const int currentUpdateState = static_cast<int>(UpdateService::GetState());
            const std::wstring currentUpdateMessage = UpdateService::GetStatusMessage();
            if (currentUpdateState != lastUpdateStateSnapshot_ ||
                currentUpdateMessage != lastUpdateStatusMessage_)
            {
                lastUpdateStateSnapshot_ = currentUpdateState;
                lastUpdateStatusMessage_ = currentUpdateMessage;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }

            // Poll external file modifications and auto-reload clean tabs.
            DWORD now = GetTickCount();
            if (now - lastExternalFileCheckTick_ >= 500)
            {
                bool reloadedAny = false;
                for (auto &kv : editors_)
                {
                    Orion::Editor *ed = kv.second;
                    if (!ed)
                        continue;
                    if (ed->ReloadFromDiskIfExternalChange(hwnd_, kv.first))
                    {
                        tabBar_.SetTabDirty(kv.first, false);
                        reloadedAny = true;
                    }
                }
                if (reloadedAny)
                    InvalidateRect(hwnd_, nullptr, FALSE);
                lastExternalFileCheckTick_ = now;
            }
            return 0;
        }
        if (wParam == EDITOR_DRAG_TIMER_ID)
        {
            Orion::Editor *editor = GetEditor();
            if (!editor || !editor->IsDragSelecting())
                return 0;
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
                return 0;
            if (GetCapture() != hwnd_)
                return 0;

            POINT pt = {0, 0};
            if (GetCursorPos(&pt))
            {
                ScreenToClient(hwnd_, &pt);
                editor->OnMouseMove(hwnd_, pt);
            }
            return 0;
        }
        if (wParam == TITLEBAR_HOVER_TIMER_ID)
        {
            StepTitlebarHoverAnimation();
            return 0;
        }
        if (wParam == DIAG_TIMER_ID)
        {
            DWORD now = GetTickCount();
            std::vector<int> ready;
            for (const auto &kv : pendingDiagTick_)
            {
                if (now - kv.second >= 200)
                    ready.push_back(kv.first);
            }

            for (int tabIndex : ready)
            {
                auto it = pendingDiagTick_.find(tabIndex);
                if (it != pendingDiagTick_.end())
                    pendingDiagTick_.erase(it);

                Orion::Editor *ed = GetEditorForTab(tabIndex);
                if (!ed)
                    continue;
                std::wstring fp = ed->GetFilePath();
                if (fp.empty() || fp.rfind(L"__untitled__", 0) == 0)
                    continue;
                auto lines = ed->GetLinesSnapshot();
                Lsp::LspManager::Instance().UpdateFile(fp, lines);
                Lsp::LspManager::Instance().RequestDiagnosticsAsync(fp, lines, hwnd_, tabIndex);
            }

            if (pendingDiagTick_.empty())
            {
                diagTimerActive_ = false;
                KillTimer(hwnd_, DIAG_TIMER_ID);
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
                  // 0: Couper, 1: Copier, 2: Coller, 3: Aller a la definition, 4: Envoyer avec ggwave
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
                  case 3: // Aller a la definition
                  {
                      if (pendingContextGoto_.has_value())
                      {
                          auto *req = new OpenFileRequest();
                          req->filePath = pendingContextGoto_->filePath;
                          req->line = pendingContextGoto_->line;
                          req->column = pendingContextGoto_->column;
                          PostMessageW(hwnd_, WM_OPEN_FILE_AT, 0, (LPARAM)req);
                          return 0;
                      }
                      break;
                  }
                  case 4: // Envoyer avec ggwave
                  {
                      std::wstring sel = editor->GetSelectionText();
                      if (!sel.empty())
                      {
                          ggwave::SpeakText(sel);
                        return 0;
                    }
                }
                  case 50: // Deplacer vers fonction (selection lignes via gouttiere)
                  {
                      if (editor->MoveSelectionToFunction())
                      {
                          InvalidateRect(hwnd_, nullptr, FALSE);
                          return 0;
                      }
                      break;
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

        if (id >= 9000 && id < 9100)
        {
            GetExplorerManager().HandleContextSubmenuCommand(id);
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
                // File menu: New, New Window, Open..., Open Recent, Open Project, Close
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
                        editor->onDocumentChanged = [this, tabIndex]()
                        {
                            tabBar_.SetTabDirty(tabIndex, true);
                            InvalidateRect(hwnd_, nullptr, FALSE);
                        };

                        if (HasTamponText())
                            editor->SetTextContent(GetTamponText(), true);
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
                case 3: // Open Recent -> handled via submenu
                {
                    return 0;
                }
                case 4: // Open Project -> pick folder and initialize Explorer
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
                                    AddRecentProject(selectedFolder);
                                    InvalidateRect(hwnd_, nullptr, FALSE);
                                }
                                pItem->Release();
                            }
                        }
                        pFileOpen->Release();
                    }
                    return 0;
                }
                case 5: // Close -> close active tab
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

            // Edit menu
            if (menu == 1)
            {
                Orion::Editor *editor = GetEditor();
                if (!editor)
                    return 0;

                switch (index)
                {
                case 0: // Undo
                    editor->Undo();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 1: // Cut
                    editor->CutSelectionToClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 2: // Copy
                    editor->CopySelectionToClipboard();
                    return 0;
                case 3: // Paste
                    editor->PasteFromClipboard();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 4: // Delete
                    editor->DeleteSelectionPublic();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 5: // Select All
                    editor->SelectAll();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                default:
                    break;
                }
            }

            // Tampon menu
            if (menu == 2)
            {
                switch (index)
                {
                case 0: // Definir Tampon...
                    keyboard_.BeginTamponEdit();
                    return 0;
                case 1: // Effacer Tampon
                    ClearTamponText();
                    Footer_SetHint(hwnd_, L"Tampon efface", 1500);
                    return 0;
                case 2: // Voir Tampon
                {
                    if (HasTamponText())
                    {
                        std::wstring preview = GetTamponText();
                        if (preview.size() > 140)
                        {
                            preview = preview.substr(0, 140);
                            preview += L"...";
                        }
                        Footer_SetHint(hwnd_, L"Tampon: " + preview, 2000);
                    }
                    else
                    {
                        Footer_SetHint(hwnd_, L"Tampon vide", 1500);
                    }
                    return 0;
                }
                default:
                    break;
                }
            }

            // Selection menu
            if (menu == 3)
            {
                Orion::Editor *editor = GetEditor();
                if (!editor)
                    return 0;

                switch (index)
                {
                case 0: // Select All
                    editor->SelectAll();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 1: // Expand Selection
                    editor->ExpandSelection();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 2: // Shrink Selection
                    editor->ShrinkSelection();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case 3: // Select Line
                    editor->SelectCurrentLine();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                default:
                    break;
                }
            }

            wchar_t buf[256];
            swprintf_s(buf, sizeof(buf) / sizeof(buf[0]), L"Menu %d item %d selected", menu, index);
            // Handle some View menu actions (menu == 4)
            if (menu == 4)
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

        // Recent projects submenu commands (8000..8999)
        if (id >= 8000 && id < 9000)
        {
            int rel = id - 8000;
            if (rel >= 0 && rel < (int)recentProjects_.size())
            {
                OpenProjectAtPath(recentProjects_[rel].path);
                return 0;
            }
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
        if (newProjectVisible_ && current == Window::Hovered_Run)
        {
            hoveredButton_ = Hovered_None;
            return 0;
        }
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
            // Build context menu in French: Couper, Copier, Coller, Aller a la definition, Envoyer avec ggwave
            pendingContextGoto_ = editor->TryGoToDefinitionAtPoint(pt);
            std::vector<std::wstring> items = {L"Couper", L"Copier", L"Coller", L"Aller a la definition", L"Envoyer avec ggwave"};
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
            enabled[2] = canPaste;                              // Coller
            enabled[3] = pendingContextGoto_.has_value();       // Aller a la definition
            enabled[4] = hasSelection;                          // Envoyer avec ggwave

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
            if (newProjectVisible_)
            {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
            }

            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);

            if (IsPointInUpdateToast(pt) && UpdateService::HasUpdateAvailable())
            {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }

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

            // Explorer keeps cursor priority over terminal when both are visible.
            if (GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt))
            {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
            }

            // Terminal resize zone (vertical)
            TerminalPanel &terminal = GetTerminalPanel();
            if (terminal.IsVisible() && terminal.IsPointInResizeZone(pt))
            {
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
                return TRUE;
            }
            if (terminal.IsVisible() && terminal.IsPointInPanel(pt))
            {
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && terminal.HasHoveredOutputLink())
                {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
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
                    if (editor->IsPointOnGitSplitDivider(pt))
                    {
                        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                        return TRUE;
                    }
                    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && editor->IsDefinitionHoverActive())
                    {
                        SetCursor(LoadCursor(NULL, IDC_HAND));
                        return TRUE;
                    }
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
    case WM_CLOSE:
    {
        // Ensure background threads/PTY sessions are stopped before exit.
        CloseActivePopupWindow();
        GetTerminalPanel().CloseAll();
        GetGGWavePanel().Shutdown();
        DestroyWindow(hwnd_);
        return 0;
    }
    case WM_DESTROY:
        CloseActivePopupWindow();
        GetTerminalPanel().CloseAll();
        GetGGWavePanel().Shutdown();
        DragAcceptFiles(hwnd_, FALSE);
        KillTimer(hwnd_, CARET_TIMER_ID);
        KillTimer(hwnd_, TITLEBAR_HOVER_TIMER_ID);
        KillTimer(hwnd_, DIAG_TIMER_ID);
        KillTimer(hwnd_, EDITOR_DRAG_TIMER_ID);
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
    settingsTab_ = std::make_unique<SettingsTabView>();
    LoadRecentProjects();
}

Window::~Window()
{
    for (auto &p : editors_)
    {
        delete p.second;
    }
    editors_.clear();
    if (uiFontCollection_)
    {
        uiFontCollection_->Release();
        uiFontCollection_ = nullptr;
    }
    if (skia_)
        delete skia_;
}

float Window::GetTitlebarHoverAlpha(CustomTitleBarHoveredButton btn) const
{
    switch (btn)
    {
    case Hovered_Minimize: return titlebarHoverMin_;
    case Hovered_Maximize: return titlebarHoverMax_;
    case Hovered_Close: return titlebarHoverClose_;
    case Hovered_Run: return titlebarHoverRun_;
    default: break;
    }
    return 0.0f;
}

void Window::StartTitlebarHoverAnimation()
{
    if (!hwnd_)
        return;
    if (!titlebarHoverAnimating_)
    {
        titlebarHoverAnimating_ = true;
        titlebarHoverLastTick_ = GetTickCount();
        SetTimer(hwnd_, TITLEBAR_HOVER_TIMER_ID, TITLEBAR_HOVER_TIMER_INTERVAL_MS, nullptr);
    }
}

void Window::StepTitlebarHoverAnimation()
{
    DWORD now = GetTickCount();
    float dt = (titlebarHoverLastTick_ == 0) ? 0.016f : (float)(now - titlebarHoverLastTick_) / 1000.0f;
    titlebarHoverLastTick_ = now;

    float targetMin = (hoveredButton_ == Hovered_Minimize) ? 1.0f : 0.0f;
    float targetMax = (hoveredButton_ == Hovered_Maximize) ? 1.0f : 0.0f;
    float targetClose = (hoveredButton_ == Hovered_Close) ? 1.0f : 0.0f;
    float targetRun = (hoveredButton_ == Hovered_Run) ? 1.0f : 0.0f;

    float speed = 18.0f;
    auto approach = [&](float current, float target) -> float
    {
        float k = std::clamp(speed * dt, 0.0f, 1.0f);
        return current + (target - current) * k;
    };

    titlebarHoverMin_ = approach(titlebarHoverMin_, targetMin);
    titlebarHoverMax_ = approach(titlebarHoverMax_, targetMax);
    titlebarHoverClose_ = approach(titlebarHoverClose_, targetClose);
    titlebarHoverRun_ = approach(titlebarHoverRun_, targetRun);

    auto isNear = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    if (isNear(titlebarHoverMin_, targetMin) &&
        isNear(titlebarHoverMax_, targetMax) &&
        isNear(titlebarHoverClose_, targetClose) &&
        isNear(titlebarHoverRun_, targetRun))
    {
        titlebarHoverAnimating_ = false;
        KillTimer(hwnd_, TITLEBAR_HOVER_TIMER_ID);
    }

    RECT tb = win32_titlebar_rect(hwnd_);
    InvalidateRect(hwnd_, &tb, FALSE);
}

void Window::ScheduleDiagnosticsForTab(int tabIndex)
{
    if (tabIndex < 0 || !hwnd_)
        return;
    pendingDiagTick_[tabIndex] = GetTickCount();
    if (!diagTimerActive_)
    {
        diagTimerActive_ = true;
        SetTimer(hwnd_, DIAG_TIMER_ID, DIAG_TIMER_INTERVAL_MS, nullptr);
    }
}

void Window::ClearAllHoverStates()
{
    hoveredButton_ = Hovered_None;
    for (size_t i = 0; i < GetMenuItems().size(); ++i)
        SetMenuItemHovered((int)i, false);
    HideMenuDropdown(hwnd_);

    POINT pt;
    bool inExplorer = false;
    if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt))
    {
        inExplorer = GetExplorerManager().IsVisible() && GetExplorerManager().IsPointInExplorer(pt);
    }
    if (!inExplorer)
        GetExplorerManager().ClearHover(hwnd_);
    GetPanelManager().ClearResizeHover(hwnd_);
    Footer_ClearHover(hwnd_);
    updateToastHovered_ = false;
    StartTitlebarHoverAnimation();

    // tabbar hover se clear ailleurs (WM_MOUSELEAVE / sortie tabbar)
    ThrottledInvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::SetUpdateToastRect(const D2D1_RECT_F &rect)
{
    updateToastRect_ = rect;
}

void Window::ClearUpdateToastRect()
{
    updateToastRect_ = D2D1::RectF(0, 0, 0, 0);
    updateToastHovered_ = false;
}

bool Window::IsPointInUpdateToast(POINT pt) const
{
    if (updateToastRect_.right <= updateToastRect_.left || updateToastRect_.bottom <= updateToastRect_.top)
        return false;
    return pt.x >= updateToastRect_.left && pt.x <= updateToastRect_.right &&
           pt.y >= updateToastRect_.top && pt.y <= updateToastRect_.bottom;
}

bool Window::SetUpdateToastHovered(bool hovered)
{
    if (updateToastHovered_ == hovered)
        return false;
    updateToastHovered_ = hovered;
    return true;
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

    UINT dpi = win32_get_dpi_for_window(hwnd_);
    int sidebarWidth = win32_dpi_scale(52, dpi);
    int panelLeftWidth = 0;
    int panelRightWidth = 0;

    Panel *activePanel = GetPanelManager().GetActivePanel();
    if (activePanel && activePanel->IsVisible())
    {
        int activeWidth = activePanel->GetPhysicalWidth();
        if (activePanel->GetId() == PanelId::Explorer &&
            GetExplorerLayoutState().placement == ExplorerPlacement::Right)
        {
            panelRightWidth = activeWidth;
        }
        else
        {
            panelLeftWidth = activeWidth;
        }
    }

    r.left = sidebarWidth + panelLeftWidth;
    r.right = client.right - panelRightWidth;

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

