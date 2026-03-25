#pragma warning(disable: 4505)
#include "core/window/Window.h"
#include "core/explorer/Explorer.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <ctime>
#include <cctype>
#include <optional>
#include <thread>
#include <functional>

namespace
{
    static std::wstring GetDefaultSourceReposPath()
    {
        PWSTR profilePath = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profilePath)) && profilePath)
        {
            std::filesystem::path base(profilePath);
            CoTaskMemFree(profilePath);
            out = (base / L"source" / L"repos").wstring();
        }
        return out;
    }

    static std::optional<std::wstring> FindReadmeMarkdown(const std::wstring &rootPath)
    {
        if (rootPath.empty())
            return std::nullopt;
        try
        {
            std::filesystem::path root(rootPath);
            if (!std::filesystem::exists(root))
                return std::nullopt;

            for (const auto &entry : std::filesystem::directory_iterator(root))
            {
                if (!entry.is_regular_file())
                    continue;
                std::wstring name = entry.path().filename().wstring();
                std::wstring lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
                if (lower == L"readme.md")
                    return entry.path().wstring();
            }
        }
        catch (...)
        {
        }
        return std::nullopt;
    }

    static std::wstring PickFolder(HWND parent)
    {
        IFileOpenDialog *pFileOpen = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
        if (FAILED(hr) || !pFileOpen)
            return L"";
        DWORD options = 0;
        if (SUCCEEDED(pFileOpen->GetOptions(&options)))
            pFileOpen->SetOptions(options | FOS_PICKFOLDERS);
        std::wstring out;
        if (SUCCEEDED(pFileOpen->Show(parent)))
        {
            IShellItem *pItem = nullptr;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem)) && pItem)
            {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
                {
                    out = pszPath;
                    CoTaskMemFree(pszPath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
        return out;
    }

    static std::filesystem::path GetRecentProjectsStorePath()
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
            out /= L"recent_projects.txt";
        }
        return out;
    }

    static std::wstring GetNebulaClangdDbDir(const std::wstring &rootPath)
    {
        if (rootPath.empty())
            return {};

        wchar_t localAppData[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return {};

        size_t key = std::hash<std::wstring>{}(rootPath);
        std::filesystem::path dir = std::filesystem::path(localAppData) /
                                    L"Nebula" /
                                    L"clangd-db" /
                                    std::to_wstring(static_cast<unsigned long long>(key));
        return dir.wstring();
    }

    static void BootstrapCompilationDatabaseAsync(const std::wstring &rootPath)
    {
        if (rootPath.empty())
            return;

        std::filesystem::path root(rootPath);
        std::error_code ec;
        if (!std::filesystem::exists(root / L"CMakeLists.txt", ec))
            return;

        std::wstring dbDir = GetNebulaClangdDbDir(rootPath);
        if (dbDir.empty())
            return;

        if (std::filesystem::exists(std::filesystem::path(dbDir) / L"compile_commands.json", ec))
            return;

        std::thread([rootPath, dbDir]() {
            std::error_code mkec;
            std::filesystem::create_directories(dbDir, mkec);

            std::wstring cmdLine =
                L"cmake -S \"" + rootPath +
                L"\" -B \"" + dbDir +
                L"\" -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";

            STARTUPINFOW si = {sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};

            if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, rootPath.c_str(), &si, &pi))
            {
                WaitForSingleObject(pi.hProcess, 45000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }).detach();
    }
}

bool Window::CreateProjectFromOverlay()
{
    std::wstring name = newProjNameInput_.GetText();
    std::wstring loc = GetDefaultSourceReposPath();
    if (name.empty() || loc.empty())
    {
        MessageBoxW(hwnd_, L"Please enter a project name.", L"New Project", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring root = loc;
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
        root.push_back(L'\\');
    root += name;

    std::wstring mainFile;
    if (CreateCppConsoleProject(root, name, mainFile))
    {
        HideNewProjectOverlay();
        GetExplorerManager().Initialize(root);
        GetExplorerManager().SetVisible(true);
        BootstrapCompilationDatabaseAsync(root);
        AddRecentProject(root);
        auto readme = FindReadmeMarkdown(root);
        if (readme.has_value())
            OpenFileInNewTabWithMarkdownPreview(*readme);
        else if (!mainFile.empty())
            OpenFileInNewTab(mainFile, -1);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

void Window::OpenProjectAtPath(const std::wstring &path)
{
    if (path.empty())
        return;
    GetExplorerManager().Initialize(path);
    GetExplorerManager().SetVisible(true);
    BootstrapCompilationDatabaseAsync(path);
    AddRecentProject(path);
    auto readme = FindReadmeMarkdown(path);
    if (readme.has_value())
        OpenFileInNewTabWithMarkdownPreview(*readme);
    HideNewProjectOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::LoadRecentProjects()
{
    recentProjects_.clear();
    recentProjectRects_.clear();
    recentProjectHover_ = -1;

    std::filesystem::path store = GetRecentProjectsStorePath();
    if (!store.empty())
    {
        std::wifstream ifs(store);
        if (ifs)
        {
            std::unordered_map<std::wstring, std::time_t> latest;
            std::wstring line;
            while (std::getline(ifs, line))
            {
                if (line.empty())
                    continue;
                std::time_t ts = 0;
                std::wstring path;
                size_t sep = line.find(L'|');
                if (sep != std::wstring::npos)
                {
                    try
                    {
                        ts = (std::time_t)std::stoll(line.substr(0, sep));
                    }
                    catch (...)
                    {
                        ts = 0;
                    }
                    path = line.substr(sep + 1);
                }
                else
                {
                    path = line;
                }
                if (path.empty())
                    continue;
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                    continue;
                auto it = latest.find(path);
                if (it == latest.end() || ts > it->second)
                    latest[path] = ts;
            }

            for (const auto &p : latest)
                recentProjects_.push_back({p.first, p.second});
        }
    }

    std::wstring defaultPath = GetDefaultSourceReposPath();
    if (!defaultPath.empty())
    {
        bool exists = false;
        for (const auto &p : recentProjects_)
        {
            if (p.path == defaultPath)
            {
                exists = true;
                break;
            }
        }
        std::error_code ec;
        if (!exists && std::filesystem::exists(defaultPath, ec))
            recentProjects_.push_back({defaultPath, 0});
    }

    std::sort(recentProjects_.begin(), recentProjects_.end(),
              [](const RecentProjectEntry &a, const RecentProjectEntry &b)
              { return a.lastOpened > b.lastOpened; });

    const size_t maxItems = 10;
    if (recentProjects_.size() > maxItems)
        recentProjects_.resize(maxItems);
}

void Window::SaveRecentProjects() const
{
    std::filesystem::path store = GetRecentProjectsStorePath();
    if (store.empty())
        return;
    std::wofstream ofs(store, std::ios::trunc);
    if (!ofs)
        return;
    for (const auto &p : recentProjects_)
    {
        ofs << (long long)p.lastOpened << L"|" << p.path << L"\n";
    }
}

void Window::AddRecentProject(const std::wstring &path)
{
    if (path.empty())
        return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return;

    std::time_t now = std::time(nullptr);
    bool found = false;
    for (auto &p : recentProjects_)
    {
        if (p.path == path)
        {
            p.lastOpened = now;
            found = true;
            break;
        }
    }
    if (!found)
        recentProjects_.push_back({path, now});

    std::sort(recentProjects_.begin(), recentProjects_.end(),
              [](const RecentProjectEntry &a, const RecentProjectEntry &b)
              { return a.lastOpened > b.lastOpened; });

    const size_t maxItems = 10;
    if (recentProjects_.size() > maxItems)
        recentProjects_.resize(maxItems);

    SaveRecentProjects();
}

bool Window::CreateCppConsoleProject(const std::wstring &rootPath, const std::wstring &projectName, std::wstring &outMainFile)
{
    try
    {
        auto toUtf8 = [](const std::wstring &w) -> std::string
        {
            if (w.empty())
                return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1)
                return {};
            std::string out(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
            return out;
        };

        namespace fs = std::filesystem;
        fs::path root(rootPath);
        if (fs::exists(root))
        {
            MessageBoxW(hwnd_, L"Project folder already exists.", L"New Project", MB_OK | MB_ICONWARNING);
            return false;
        }

        fs::create_directories(root / ".nebula");

        std::string nameUtf8 = toUtf8(projectName);
        auto toTargetName = [](std::string value) -> std::string
        {
            for (auto &c : value)
            {
                if (!std::isalnum(static_cast<unsigned char>(c)))
                    c = '_';
            }
            if (value.empty())
                value = "app";
            return value;
        };
        std::string targetName = toTargetName(nameUtf8);
        std::string projType = "cpp-console";
        std::string entryPath = "src/main.cpp";
        fs::path mainPath;
        bool needsCMake = false;

        if (newProjTemplateIndex_ == 0)
        {
            fs::create_directories(root / "src");
            fs::create_directories(root / "include");
            mainPath = root / "src" / "main.cpp";
            needsCMake = true;
            std::ofstream ofs(mainPath, std::ios::binary);
            ofs << "#include <iostream>\n\n";
            ofs << "int main() {\n";
            ofs << "    std::cout << \"Hello from " << nameUtf8 << "!\" << std::endl;\n";
            ofs << "    return 0;\n";
            ofs << "}\n";
        }
        else if (newProjTemplateIndex_ == 1)
        {
            fs::create_directories(root / "src");
            fs::create_directories(root / "include");
            projType = "cpp-win32";
            mainPath = root / "src" / "main.cpp";
            needsCMake = true;
            std::ofstream ofs(mainPath, std::ios::binary);
            ofs << "#include <windows.h>\n\n";
            ofs << "static const wchar_t *kClassName = L\"NebulaWin32Window\";\n\n";
            ofs << "LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {\n";
            ofs << "    switch (msg) {\n";
            ofs << "    case WM_DESTROY:\n";
            ofs << "        PostQuitMessage(0);\n";
            ofs << "        return 0;\n";
            ofs << "    default:\n";
            ofs << "        return DefWindowProcW(hwnd, msg, wParam, lParam);\n";
            ofs << "    }\n";
            ofs << "}\n\n";
            ofs << "int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {\n";
            ofs << "    WNDCLASSW wc = {};\n";
            ofs << "    wc.lpfnWndProc = WndProc;\n";
            ofs << "    wc.hInstance = hInstance;\n";
            ofs << "    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);\n";
            ofs << "    wc.lpszClassName = kClassName;\n";
            ofs << "    RegisterClassW(&wc);\n\n";
            ofs << "    HWND hwnd = CreateWindowExW(0, kClassName, L\"Win32 App\",\n";
            ofs << "        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,\n";
            ofs << "        nullptr, nullptr, hInstance, nullptr);\n";
            ofs << "    if (!hwnd) return 0;\n";
            ofs << "    ShowWindow(hwnd, nCmdShow);\n\n";
            ofs << "    MSG msg;\n";
            ofs << "    while (GetMessageW(&msg, nullptr, 0, 0)) {\n";
            ofs << "        TranslateMessage(&msg);\n";
            ofs << "        DispatchMessageW(&msg);\n";
            ofs << "    }\n";
            ofs << "    return (int)msg.wParam;\n";
            ofs << "}\n";
        }
        else if (newProjTemplateIndex_ == 2)
        {
            fs::create_directories(root / "src");
            fs::create_directories(root / "include");
            projType = "cpp-library";
            needsCMake = true;
            std::wstring targetNameW(targetName.begin(), targetName.end());
            fs::path headerPath = root / "include" / (targetNameW + L".h");
            fs::path implPath = root / "src" / (targetNameW + L".cpp");
            {
                std::ofstream ofs(headerPath, std::ios::binary);
                ofs << "#pragma once\n\n";
                ofs << "int add(int a, int b);\n";
            }
            {
                std::ofstream ofs(implPath, std::ios::binary);
                ofs << "#include \"" << targetName << ".h\"\n\n";
                ofs << "int add(int a, int b) {\n";
                ofs << "    return a + b;\n";
                ofs << "}\n";
            }
            mainPath = implPath;
            entryPath = "src/" + targetName + ".cpp";
        }
        else if (newProjTemplateIndex_ == 3)
        {
            fs::create_directories(root / "src");
            projType = "python";
            mainPath = root / "src" / "main.py";
            std::ofstream ofs(mainPath, std::ios::binary);
            ofs << "def main():\n";
            ofs << "    print(\"Hello from " << nameUtf8 << "!\")\n\n";
            ofs << "if __name__ == \"__main__\":\n";
            ofs << "    main()\n";
            entryPath = "src/main.py";
        }
        else if (newProjTemplateIndex_ == 4)
        {
            fs::create_directories(root / "src");
            projType = "web";
            fs::path htmlPath = root / "src" / "index.html";
            fs::path cssPath = root / "src" / "style.css";
            fs::path jsPath = root / "src" / "app.js";
            {
                std::ofstream ofs(htmlPath, std::ios::binary);
                ofs << "<!doctype html>\n";
                ofs << "<html lang=\"fr\">\n";
                ofs << "<head>\n";
                ofs << "  <meta charset=\"utf-8\" />\n";
                ofs << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n";
                ofs << "  <title>" << nameUtf8 << "</title>\n";
                ofs << "  <link rel=\"stylesheet\" href=\"style.css\" />\n";
                ofs << "</head>\n";
                ofs << "<body>\n";
                ofs << "  <main>\n";
                ofs << "    <h1>" << nameUtf8 << "</h1>\n";
                ofs << "    <p>Votre projet web est pret.</p>\n";
                ofs << "  </main>\n";
                ofs << "  <script src=\"app.js\"></script>\n";
                ofs << "</body>\n";
                ofs << "</html>\n";
            }
            {
                std::ofstream ofs(cssPath, std::ios::binary);
                ofs << "body {\n";
                ofs << "  font-family: system-ui, sans-serif;\n";
                ofs << "  margin: 0;\n";
                ofs << "  padding: 40px;\n";
                ofs << "  background: #0f1115;\n";
                ofs << "  color: #e6e6e6;\n";
                ofs << "}\n";
                ofs << "main {\n";
                ofs << "  max-width: 720px;\n";
                ofs << "}\n";
            }
            {
                std::ofstream ofs(jsPath, std::ios::binary);
                ofs << "console.log(\"" << nameUtf8 << " ready\");\n";
            }
            mainPath = htmlPath;
            entryPath = "src/index.html";
        }
        else
        {
            return false;
        }

        if (needsCMake)
        {
            fs::path cmakePath = root / "CMakeLists.txt";
            std::ofstream ofs(cmakePath, std::ios::binary);
            ofs << "cmake_minimum_required(VERSION 3.20)\n";
            ofs << "project(" << targetName << " LANGUAGES CXX)\n\n";
            ofs << "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n";
            ofs << "set(CMAKE_CXX_STANDARD 17)\n";
            ofs << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
            if (projType == "cpp-console")
            {
                ofs << "add_executable(" << targetName << " src/main.cpp)\n";
                ofs << "target_include_directories(" << targetName << " PRIVATE include)\n";
            }
            else if (projType == "cpp-win32")
            {
                ofs << "add_executable(" << targetName << " WIN32 src/main.cpp)\n";
                ofs << "target_include_directories(" << targetName << " PRIVATE include)\n";
            }
            else if (projType == "cpp-library")
            {
                ofs << "add_library(" << targetName << " STATIC src/" << targetName << ".cpp)\n";
                ofs << "target_include_directories(" << targetName << " PUBLIC include)\n";
            }
        }

        // project config
        fs::path projPath = root / ".nebula" / "project.json";
        {
            std::ofstream ofs(projPath, std::ios::binary);
            ofs << "{\n";
            ofs << "  \"name\": \"" << nameUtf8 << "\",\n";
            ofs << "  \"type\": \"" << projType << "\",\n";
            ofs << "  \"version\": 1,\n";
            ofs << "  \"sourceRoot\": \"src\",\n";
            ofs << "  \"entry\": \"" << entryPath << "\"\n";
            ofs << "}\n";
        }

        outMainFile = mainPath.wstring();
        return true;
    }
    catch (...)
    {
        MessageBoxW(hwnd_, L"Failed to create project files.", L"New Project", MB_OK | MB_ICONERROR);
        return false;
    }
}

void Window::OpenProjectDialog()
{
    IFileOpenDialog *pFileOpen = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
    if (SUCCEEDED(hr) && pFileOpen)
    {
        DWORD options = 0;
        if (SUCCEEDED(pFileOpen->GetOptions(&options)))
            pFileOpen->SetOptions(options | FOS_PICKFOLDERS);

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
}
