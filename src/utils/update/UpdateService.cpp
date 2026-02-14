#include "utils/update/UpdateService.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kApiHost[] = L"api.astracode.dev";
    constexpr wchar_t kApiBaseUrl[] = L"https://api.astracode.dev";
    constexpr wchar_t kUpdatePathBase[] = L"/versions/update?current_version=";

#ifndef NEBULA_APP_VERSION
#define NEBULA_APP_VERSION "1.0.0"
#endif

    constexpr const char *kCurrentVersionUtf8 = NEBULA_APP_VERSION;

    struct SharedState
    {
        std::mutex mutex;
        bool initialized = false;
        std::atomic<bool> checking{false};
        UpdateService::State state = UpdateService::State::Idle;
        UpdateService::LatestInfo latest;
        std::wstring message = L"No update check has run yet.";
    };

    SharedState g_state;

    std::wstring Utf8ToWide(const std::string &text)
    {
        if (text.empty())
            return {};
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            return {};
        std::wstring out((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len);
        return out;
    }

    std::string WideToUtf8(const std::wstring &text)
    {
        if (text.empty())
            return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return {};
        std::string out((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::wstring Trim(const std::wstring &text)
    {
        size_t a = 0;
        while (a < text.size() && iswspace(text[a]))
            ++a;
        size_t b = text.size();
        while (b > a && iswspace(text[b - 1]))
            --b;
        return text.substr(a, b - a);
    }

    std::string UrlEncode(const std::string &value)
    {
        std::string out;
        out.reserve(value.size());
        static const char *hex = "0123456789ABCDEF";
        for (unsigned char ch : value)
        {
            const bool isUnreserved = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
            if (isUnreserved)
            {
                out.push_back((char)ch);
                continue;
            }
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0x0F]);
            out.push_back(hex[ch & 0x0F]);
        }
        return out;
    }

    bool HttpGetLatest(std::string &outBody, std::wstring &outError)
    {
        outBody.clear();
        outError.clear();

        std::wstring agent = L"Nebula/";
        agent += Utf8ToWide(std::string(kCurrentVersionUtf8));

        HINTERNET hSession = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
        {
            outError = L"Unable to start WinHTTP session.";
            return false;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            outError = L"Unable to connect to update server.";
            return false;
        }

        const std::string encodedVersion = UrlEncode(std::string(kCurrentVersionUtf8));
        const std::wstring requestPath = std::wstring(kUpdatePathBase) + Utf8ToWide(encodedVersion);

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", requestPath.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Unable to create update request.";
            return false;
        }

        const wchar_t *headers = L"Accept: application/json\r\n";
        BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok)
            ok = WinHttpReceiveResponse(hRequest, nullptr);

        if (!ok)
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Update request failed.";
            return false;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &statusCode,
                                 &statusCodeSize,
                                 WINHTTP_NO_HEADER_INDEX))
        {
            statusCode = 0;
        }

        if (statusCode < 200 || statusCode >= 300)
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Update server returned HTTP " + std::to_wstring((unsigned long long)statusCode) + L".";
            return false;
        }

        DWORD available = 0;
        do
        {
            if (!WinHttpQueryDataAvailable(hRequest, &available))
                break;
            if (available == 0)
                break;
            std::vector<char> chunk((size_t)available);
            DWORD downloaded = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), available, &downloaded))
                break;
            outBody.append(chunk.data(), chunk.data() + downloaded);
        } while (available > 0);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (outBody.empty())
        {
            outError = L"Update server returned an empty response.";
            return false;
        }
        return true;
    }

    bool ParseJsonStringAt(const std::string &json, size_t openingQuotePos, std::string &out)
    {
        out.clear();
        if (openingQuotePos >= json.size() || json[openingQuotePos] != '"')
            return false;

        bool escape = false;
        for (size_t i = openingQuotePos + 1; i < json.size(); ++i)
        {
            char c = json[i];
            if (escape)
            {
                switch (c)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(c);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    // Keep unsupported unicode escape as placeholder.
                    out.push_back('?');
                    if (i + 4 < json.size())
                        i += 4;
                    break;
                default:
                    out.push_back(c);
                    break;
                }
                escape = false;
                continue;
            }
            if (c == '\\')
            {
                escape = true;
                continue;
            }
            if (c == '"')
                return true;
            out.push_back(c);
        }

        out.clear();
        return false;
    }

    bool ExtractJsonString(const std::string &json, const std::string &key, std::string &out)
    {
        out.clear();
        const std::string needle = "\"" + key + "\"";
        size_t pos = 0;
        while (true)
        {
            pos = json.find(needle, pos);
            if (pos == std::string::npos)
                return false;

            size_t colon = json.find(':', pos + needle.size());
            if (colon == std::string::npos)
                return false;

            size_t valuePos = colon + 1;
            while (valuePos < json.size() && std::isspace((unsigned char)json[valuePos]))
                ++valuePos;
            if (valuePos >= json.size())
                return false;

            if (json[valuePos] == '"')
                return ParseJsonStringAt(json, valuePos, out);

            pos += needle.size();
        }
    }

    bool ExtractJsonObject(const std::string &json, const std::string &key, std::string &out)
    {
        out.clear();
        const std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return false;

        size_t colon = json.find(':', pos + needle.size());
        if (colon == std::string::npos)
            return false;

        size_t valuePos = colon + 1;
        while (valuePos < json.size() && std::isspace((unsigned char)json[valuePos]))
            ++valuePos;
        if (valuePos >= json.size() || json[valuePos] != '{')
            return false;

        int depth = 0;
        bool inString = false;
        bool escape = false;
        for (size_t i = valuePos; i < json.size(); ++i)
        {
            char c = json[i];
            if (inString)
            {
                if (escape)
                {
                    escape = false;
                    continue;
                }
                if (c == '\\')
                {
                    escape = true;
                    continue;
                }
                if (c == '"')
                    inString = false;
                continue;
            }

            if (c == '"')
            {
                inString = true;
                continue;
            }
            if (c == '{')
                ++depth;
            else if (c == '}')
            {
                --depth;
                if (depth == 0)
                {
                    out = json.substr(valuePos, i - valuePos + 1);
                    return true;
                }
            }
        }
        return false;
    }

    std::wstring ToAbsoluteDownloadUrl(const std::wstring &value)
    {
        std::wstring trimmed = Trim(value);
        if (trimmed.empty())
            return {};
        if (trimmed.rfind(L"http://", 0) == 0 || trimmed.rfind(L"https://", 0) == 0)
            return trimmed;
        if (trimmed.front() == L'/')
            return std::wstring(kApiBaseUrl) + trimmed;
        return std::wstring(kApiBaseUrl) + L"/" + trimmed;
    }

    bool DownloadFileToPath(const std::wstring &url, const std::wstring &destinationPath, std::wstring &outError)
    {
        outError.clear();
        if (url.empty())
        {
            outError = L"Missing download URL.";
            return false;
        }

        URL_COMPONENTS components = {};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = (DWORD)-1;
        components.dwUrlPathLength = (DWORD)-1;
        components.dwExtraInfoLength = (DWORD)-1;
        components.dwSchemeLength = (DWORD)-1;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
        {
            outError = L"Invalid setup URL.";
            return false;
        }

        std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        const bool isSecure = components.nScheme == INTERNET_SCHEME_HTTPS;
        HINTERNET hSession = WinHttpOpen(L"Nebula-Updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
        {
            outError = L"Unable to start WinHTTP session for download.";
            return false;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), components.nPort, 0);
        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            outError = L"Unable to connect to setup host.";
            return false;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                isSecure ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Unable to create setup download request.";
            return false;
        }

        const wchar_t *headers = L"Accept: */*\r\n";
        BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok)
            ok = WinHttpReceiveResponse(hRequest, nullptr);
        if (!ok)
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Setup download request failed.";
            return false;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode,
                            &statusCodeSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (statusCode < 200 || statusCode >= 300)
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Setup download failed with HTTP " + std::to_wstring((unsigned long long)statusCode) + L".";
            return false;
        }

        HANDLE file = CreateFileW(destinationPath.c_str(),
                                  GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"Unable to create temporary setup file.";
            return false;
        }

        bool wroteAny = false;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
        {
            std::vector<char> buffer((size_t)available);
            DWORD downloaded = 0;
            if (!WinHttpReadData(hRequest, buffer.data(), available, &downloaded))
            {
                CloseHandle(file);
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                outError = L"Error while downloading setup file.";
                return false;
            }
            if (downloaded == 0)
                break;

            DWORD written = 0;
            if (!WriteFile(file, buffer.data(), downloaded, &written, nullptr) || written != downloaded)
            {
                CloseHandle(file);
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                outError = L"Error while writing setup file.";
                return false;
            }
            wroteAny = true;
        }

        CloseHandle(file);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (!wroteAny)
        {
            outError = L"Downloaded setup file is empty.";
            return false;
        }
        return true;
    }

    std::wstring BuildTempSetupPath(const UpdateService::LatestInfo &info)
    {
        wchar_t tempPath[MAX_PATH] = {};
        DWORD len = GetTempPathW((DWORD)std::size(tempPath), tempPath);
        if (len == 0 || len >= std::size(tempPath))
            return {};

        std::filesystem::path dir = std::filesystem::path(tempPath) / L"NebulaUpdater";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::wstring suffix = info.version.empty() ? L"latest" : info.version;
        for (wchar_t &ch : suffix)
        {
            if (!(iswalnum(ch) || ch == L'.' || ch == L'-' || ch == L'_'))
                ch = L'_';
        }

        std::filesystem::path setupPath = dir / (L"Nebula-" + suffix + L"-setup.exe");
        return setupPath.wstring();
    }

    bool WriteUpdaterScript(const std::wstring &scriptPath,
                            const std::wstring &setupPath,
                            const std::wstring &exePath,
                            DWORD currentPid,
                            std::wstring &outError)
    {
        std::ofstream ofs(std::filesystem::path(scriptPath), std::ios::binary | std::ios::trunc);
        if (!ofs)
        {
            outError = L"Unable to create updater script.";
            return false;
        }

        const std::string setupUtf8 = WideToUtf8(setupPath);
        const std::string exeUtf8 = WideToUtf8(exePath);
        const std::string pid = std::to_string((unsigned long long)currentPid);

        ofs << "@echo off\r\n";
        ofs << "setlocal\r\n";
        ofs << ":wait_exit\r\n";
        ofs << "tasklist /FI \"PID eq " << pid << "\" 2>NUL | find \"" << pid << "\" >NUL\r\n";
        ofs << "if not errorlevel 1 (\r\n";
        ofs << "  timeout /t 1 /nobreak >NUL\r\n";
        ofs << "  goto wait_exit\r\n";
        ofs << ")\r\n";
        ofs << "start \"\" /wait \"" << setupUtf8 << "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-\r\n";
        ofs << "start \"\" \"" << exeUtf8 << "\"\r\n";
        ofs << "del /f /q \"" << setupUtf8 << "\" >NUL 2>&1\r\n";
        ofs << "del /f /q \"%~f0\" >NUL 2>&1\r\n";

        if (!ofs.good())
        {
            outError = L"Unable to write updater script.";
            return false;
        }
        return true;
    }

    std::wstring BuildUpdaterScriptPath()
    {
        wchar_t tempPath[MAX_PATH] = {};
        DWORD len = GetTempPathW((DWORD)std::size(tempPath), tempPath);
        if (len == 0 || len >= std::size(tempPath))
            return {};

        std::filesystem::path dir = std::filesystem::path(tempPath) / L"NebulaUpdater";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        const ULONGLONG now = GetTickCount64();
        std::filesystem::path scriptPath = dir / (L"apply_update_" + std::to_wstring(now) + L".cmd");
        return scriptPath.wstring();
    }

    bool MessageIndicatesUpdateAvailable(const std::wstring &message)
    {
        std::wstring lower = Trim(message);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c)
                       { return (wchar_t)towlower(c); });
        return lower.find(L"nouvelle version disponible") != std::wstring::npos;
    }

    std::vector<int> ParseVersionParts(const std::wstring &version)
    {
        std::vector<int> parts;
        int current = 0;
        bool inDigits = false;

        for (wchar_t ch : version)
        {
            if (ch >= L'0' && ch <= L'9')
            {
                inDigits = true;
                current = current * 10 + (int)(ch - L'0');
            }
            else
            {
                if (inDigits)
                {
                    parts.push_back(current);
                    current = 0;
                    inDigits = false;
                }
                if (ch != L'.' && !parts.empty())
                    break;
            }
        }

        if (inDigits)
            parts.push_back(current);
        if (parts.empty())
            parts.push_back(0);
        return parts;
    }

    int CompareVersions(const std::wstring &a, const std::wstring &b)
    {
        std::vector<int> pa = ParseVersionParts(a);
        std::vector<int> pb = ParseVersionParts(b);
        const size_t count = (std::max)(pa.size(), pb.size());
        pa.resize(count, 0);
        pb.resize(count, 0);

        for (size_t i = 0; i < count; ++i)
        {
            if (pa[i] < pb[i])
                return -1;
            if (pa[i] > pb[i])
                return 1;
        }
        return 0;
    }

    bool ParseLatestPayload(const std::string &json,
                            UpdateService::LatestInfo &outInfo,
                            std::wstring &outServerMessage,
                            bool &outServerSaysUpdate,
                            std::wstring &outError)
    {
        outInfo = {};
        outServerMessage.clear();
        outServerSaysUpdate = false;
        outError.clear();

        std::string serverMessage;
        std::string version;
        std::string note;
        std::string uploadDate;
        std::string setup;
        std::string portable;
        std::string setupDirect;
        std::string portableDirect;
        std::string downloadObject;

        ExtractJsonString(json, "message", serverMessage);
        ExtractJsonString(json, "note_version", note);
        ExtractJsonString(json, "upload_date", uploadDate);
        ExtractJsonString(json, "version", version);
        if (version.empty())
            ExtractJsonString(json, "latest_version", version);
        if (version.empty())
            ExtractJsonString(json, "new_version", version);

        ExtractJsonString(json, "url_setup", setupDirect);
        ExtractJsonString(json, "url_portable", portableDirect);

        if (ExtractJsonObject(json, "download_urls", downloadObject))
        {
            ExtractJsonString(downloadObject, "portable", portable);
            ExtractJsonString(downloadObject, "setup", setup);
        }

        if (setup.empty())
            setup = setupDirect;
        if (portable.empty())
            portable = portableDirect;

        if (serverMessage.empty() && version.empty() && setup.empty() && portable.empty())
        {
            outError = L"Invalid update payload.";
            return false;
        }

        outInfo.version = Utf8ToWide(version);
        outInfo.noteVersion = Utf8ToWide(note);
        outInfo.uploadDate = Utf8ToWide(uploadDate);
        outInfo.portableUrl = ToAbsoluteDownloadUrl(Utf8ToWide(portable));
        outInfo.setupUrl = ToAbsoluteDownloadUrl(Utf8ToWide(setup));
        outServerMessage = Utf8ToWide(serverMessage);
        outServerSaysUpdate = MessageIndicatesUpdateAvailable(outServerMessage);
        return true;
    }

    std::wstring BuildUpToDateMessage(const UpdateService::LatestInfo &latest)
    {
        std::wstring msg = L"You are on the latest version (" + UpdateService::GetCurrentVersion() + L").";
        if (!latest.version.empty())
            msg += L" Latest: " + latest.version + L".";
        return msg;
    }

    std::wstring BuildUpdateAvailableMessage(const UpdateService::LatestInfo &latest)
    {
        return L"Update available: " + latest.version + L" (current: " + UpdateService::GetCurrentVersion() + L").";
    }

    void RunCheckWorker()
    {
        std::string payload;
        std::wstring error;
        std::wstring serverMessage;
        bool serverSaysUpdate = false;
        UpdateService::LatestInfo latest;

        bool ok = HttpGetLatest(payload, error);
        if (ok)
            ok = ParseLatestPayload(payload, latest, serverMessage, serverSaysUpdate, error);

        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            if (!ok)
            {
                g_state.state = UpdateService::State::Error;
                g_state.message = error.empty() ? L"Unable to check updates." : error;
            }
            else
            {
                g_state.latest = latest;
                bool hasUpdate = serverSaysUpdate;
                if (!hasUpdate && !latest.version.empty())
                    hasUpdate = CompareVersions(UpdateService::GetCurrentVersion(), latest.version) < 0;

                if (hasUpdate)
                {
                    g_state.state = UpdateService::State::UpdateAvailable;
                    g_state.message = !serverMessage.empty() ? serverMessage : BuildUpdateAvailableMessage(latest);
                }
                else
                {
                    g_state.state = UpdateService::State::UpToDate;
                    g_state.message = !serverMessage.empty() ? serverMessage : BuildUpToDateMessage(latest);
                }
            }
        }

        g_state.checking.store(false);
    }
}

void UpdateService::EnsureInitialized()
{
    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (!g_state.initialized)
        {
            g_state.initialized = true;
            shouldStart = true;
        }
    }
    if (shouldStart)
        RefreshAsync();
}

void UpdateService::RefreshAsync()
{
    bool expected = false;
    if (!g_state.checking.compare_exchange_strong(expected, true))
        return;

    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.state = State::Checking;
        g_state.message = L"Checking for updates...";
    }

    std::thread worker(RunCheckWorker);
    worker.detach();
}

UpdateService::State UpdateService::GetState()
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.state;
}

bool UpdateService::HasUpdateAvailable()
{
    return GetState() == State::UpdateAvailable;
}

std::wstring UpdateService::GetStatusMessage()
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.message;
}

std::wstring UpdateService::GetCurrentVersion()
{
    static const std::wstring kVersion = Utf8ToWide(std::string(kCurrentVersionUtf8));
    return kVersion;
}

UpdateService::LatestInfo UpdateService::GetLatestInfo()
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.latest;
}

bool UpdateService::OpenPreferredDownload(std::wstring &outError)
{
    outError.clear();
    LatestInfo info = GetLatestInfo();

    std::wstring url = info.setupUrl;
    if (url.empty())
        url = info.portableUrl;
    if (url.empty())
    {
        outError = L"No download URL available. Run update check first.";
        return false;
    }

    if ((INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
    {
        outError = L"Unable to open download URL.";
        return false;
    }
    return true;
}

bool UpdateService::InstallUpdateAndRestart(std::wstring &outError)
{
    outError.clear();
    LatestInfo info = GetLatestInfo();
    if (info.setupUrl.empty())
    {
        outError = L"No setup URL available for automatic update.";
        return false;
    }

    std::wstring setupPath = BuildTempSetupPath(info);
    if (setupPath.empty())
    {
        outError = L"Unable to resolve temp path for setup download.";
        return false;
    }

    if (!DownloadFileToPath(info.setupUrl, setupPath, outError))
        return false;

    wchar_t exePathBuffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePathBuffer, (DWORD)std::size(exePathBuffer)) == 0)
    {
        outError = L"Unable to resolve current executable path.";
        return false;
    }
    std::wstring exePath = exePathBuffer;

    const DWORD currentPid = GetCurrentProcessId();
    std::wstring scriptPath = BuildUpdaterScriptPath();
    if (scriptPath.empty())
    {
        outError = L"Unable to resolve updater script path.";
        return false;
    }

    if (!WriteUpdaterScript(scriptPath, setupPath, exePath, currentPid, outError))
        return false;

    std::wstring args = L"/C \"";
    args += scriptPath;
    args += L"\"";

    HINSTANCE launched = ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), nullptr, SW_HIDE);
    if ((INT_PTR)launched <= 32)
    {
        outError = L"Unable to start updater helper.";
        return false;
    }

    return true;
}
