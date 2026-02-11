#include "utils/auth/GitHubOAuth.h"

#include "utils/auth/GitHubAuth.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace
{
    std::wstring Trim(const std::wstring &s)
    {
        size_t a = 0;
        while (a < s.size() && iswspace(s[a]))
            ++a;
        size_t b = s.size();
        while (b > a && iswspace(s[b - 1]))
            --b;
        return s.substr(a, b - a);
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

    std::wstring Utf8ToWide(const std::string &text)
    {
        if (text.empty())
            return {};
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            return {};
        std::wstring out((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), len);
        return out;
    }

    bool ReadEnvFileValue(const std::wstring &key, std::wstring &outValue)
    {
        outValue.clear();
        wchar_t modulePath[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            return false;

        std::filesystem::path dir = std::filesystem::path(modulePath).parent_path();
        std::filesystem::path envPath;
        for (int i = 0; i < 8; ++i)
        {
            std::filesystem::path candidate = dir / L".env";
            if (std::filesystem::exists(candidate))
            {
                envPath = candidate;
                break;
            }
            if (!dir.has_parent_path())
                break;
            dir = dir.parent_path();
        }

        if (envPath.empty())
            return false;

        std::ifstream in(envPath);
        if (!in.is_open())
            return false;

        std::string keyUtf8 = WideToUtf8(key);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            auto trimAscii = [](std::string &x)
            {
                auto notSpace = [](unsigned char c) { return !std::isspace(c); };
                x.erase(x.begin(), std::find_if(x.begin(), x.end(), notSpace));
                x.erase(std::find_if(x.rbegin(), x.rend(), notSpace).base(), x.end());
            };
            trimAscii(k);
            trimAscii(v);
            if (k != keyUtf8)
                continue;
            if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
                v = v.substr(1, v.size() - 2);
            outValue = Trim(Utf8ToWide(v));
            return !outValue.empty();
        }

        return false;
    }

    bool ReadEnvVar(const wchar_t *name, std::wstring &outValue)
    {
        outValue.clear();
        DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (needed == 0)
            return false;
        std::wstring buffer((size_t)needed, L'\0');
        DWORD written = GetEnvironmentVariableW(name, buffer.data(), needed);
        if (written == 0)
            return false;
        if (!buffer.empty() && buffer.back() == L'\0')
            buffer.pop_back();
        outValue = Trim(buffer);
        return !outValue.empty();
    }

    bool LoadConfig(std::wstring &clientId, std::wstring &clientSecret, std::wstring &redirectUri, std::wstring &outError)
    {
        clientId.clear();
        clientSecret.clear();
        redirectUri.clear();
        outError.clear();

        if (!ReadEnvFileValue(L"CLIENT_ID_GITHUB", clientId))
            ReadEnvVar(L"CLIENT_ID_GITHUB", clientId);
        if (!ReadEnvFileValue(L"CLIENT_SECRET_GITHUB", clientSecret))
            ReadEnvVar(L"CLIENT_SECRET_GITHUB", clientSecret);
        if (!ReadEnvFileValue(L"GITHUB_CALLBACK_URL", redirectUri))
            ReadEnvVar(L"GITHUB_CALLBACK_URL", redirectUri);

        if (redirectUri.empty())
            redirectUri = L"http://localhost:8080/auth/github/callback";

        if (clientId.empty())
        {
            outError = L"CLIENT_ID_GITHUB missing in .env.";
            return false;
        }
        if (clientSecret.empty())
        {
            outError = L"CLIENT_SECRET_GITHUB missing in .env.";
            return false;
        }
        return true;
    }

    std::wstring UrlEncode(const std::wstring &value)
    {
        std::string utf8 = WideToUtf8(value);
        std::ostringstream oss;
        const char *hex = "0123456789ABCDEF";
        for (unsigned char c : utf8)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
            {
                oss << (char)c;
            }
            else
            {
                oss << '%' << hex[(c >> 4) & 0xF] << hex[c & 0xF];
            }
        }
        return Utf8ToWide(oss.str());
    }

    std::wstring UrlDecode(const std::wstring &value)
    {
        std::string in = WideToUtf8(value);
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            char c = in[i];
            if (c == '%' && i + 2 < in.size())
            {
                auto hexVal = [](char x) -> int
                {
                    if (x >= '0' && x <= '9')
                        return x - '0';
                    if (x >= 'a' && x <= 'f')
                        return 10 + (x - 'a');
                    if (x >= 'A' && x <= 'F')
                        return 10 + (x - 'A');
                    return -1;
                };
                int hi = hexVal(in[i + 1]);
                int lo = hexVal(in[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out.push_back((char)((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            if (c == '+')
                out.push_back(' ');
            else
                out.push_back(c);
        }
        return Utf8ToWide(out);
    }

    std::wstring GenerateState()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        unsigned long long a = dist(gen);
        unsigned long long b = dist(gen);
        wchar_t buf[40] = {0};
        swprintf_s(buf, L"%016llx%016llx", a, b);
        return std::wstring(buf);
    }

    bool ExtractQueryParams(const std::wstring &urlPath, std::map<std::wstring, std::wstring> &out)
    {
        out.clear();
        size_t q = urlPath.find(L'?');
        if (q == std::wstring::npos)
            return false;
        std::wstring query = urlPath.substr(q + 1);
        size_t start = 0;
        while (start < query.size())
        {
            size_t amp = query.find(L'&', start);
            std::wstring pair = query.substr(start, amp == std::wstring::npos ? std::wstring::npos : amp - start);
            size_t eq = pair.find(L'=');
            std::wstring k = UrlDecode(eq == std::wstring::npos ? pair : pair.substr(0, eq));
            std::wstring v = UrlDecode(eq == std::wstring::npos ? L"" : pair.substr(eq + 1));
            if (!k.empty())
                out[k] = v;
            if (amp == std::wstring::npos)
                break;
            start = amp + 1;
        }
        return true;
    }

    bool WaitForCallback(unsigned short listenPort,
                         const std::wstring &expectedPath,
                         const std::wstring &expectedState,
                         std::wstring &outCode,
                         std::wstring &outError)
    {
        outCode.clear();
        outError.clear();

        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            outError = L"WSAStartup failed.";
            return false;
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET)
        {
            outError = L"Socket creation failed.";
            WSACleanup();
            return false;
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(listenPort);

        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

        if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) != 0)
        {
            outError = L"Callback port unavailable.";
            closesocket(listenSock);
            WSACleanup();
            return false;
        }

        if (listen(listenSock, 1) != 0)
        {
            outError = L"Listen failed.";
            closesocket(listenSock);
            WSACleanup();
            return false;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listenSock, &fds);
        timeval tv = {};
        tv.tv_sec = 180;
        tv.tv_usec = 0;
        int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel <= 0)
        {
            outError = L"OAuth timeout waiting for callback.";
            closesocket(listenSock);
            WSACleanup();
            return false;
        }

        SOCKET client = accept(listenSock, nullptr, nullptr);
        closesocket(listenSock);
        if (client == INVALID_SOCKET)
        {
            outError = L"Callback accept failed.";
            WSACleanup();
            return false;
        }

        char requestBuf[8192] = {0};
        int n = recv(client, requestBuf, sizeof(requestBuf) - 1, 0);
        if (n <= 0)
        {
            outError = L"Invalid callback request.";
            closesocket(client);
            WSACleanup();
            return false;
        }
        requestBuf[n] = '\0';
        std::string req(requestBuf);
        size_t lineEnd = req.find("\r\n");
        std::string line = (lineEnd == std::string::npos) ? req : req.substr(0, lineEnd);

        size_t sp1 = line.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
        std::wstring pathWide;
        if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 > sp1 + 1)
            pathWide = Utf8ToWide(line.substr(sp1 + 1, sp2 - sp1 - 1));

        const char *okResponse =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
            "<html><body style='font-family:Segoe UI;background:#111;color:#eee;padding:24px'>"
            "<h3>GitHub connected. You can close this tab.</h3></body></html>";
        send(client, okResponse, (int)strlen(okResponse), 0);
        closesocket(client);
        WSACleanup();

        std::map<std::wstring, std::wstring> params;
        if (pathWide.empty() || !ExtractQueryParams(pathWide, params))
        {
            outError = L"Missing OAuth callback query.";
            return false;
        }

        size_t pathOnlyEnd = pathWide.find(L'?');
        std::wstring pathOnly = pathOnlyEnd == std::wstring::npos ? pathWide : pathWide.substr(0, pathOnlyEnd);
        if (pathOnly != expectedPath)
        {
            outError = L"Unexpected OAuth callback path.";
            return false;
        }

        auto itCode = params.find(L"code");
        auto itState = params.find(L"state");
        if (itCode == params.end() || itCode->second.empty())
        {
            outError = L"Missing OAuth code.";
            return false;
        }
        if (itState == params.end() || itState->second != expectedState)
        {
            outError = L"Invalid OAuth state.";
            return false;
        }

        outCode = itCode->second;
        return true;
    }

    bool ExchangeCodeForToken(const std::wstring &clientId,
                              const std::wstring &clientSecret,
                              const std::wstring &code,
                              const std::wstring &redirectUri,
                              const std::wstring &state,
                              std::wstring &outToken,
                              std::wstring &outError)
    {
        outToken.clear();
        outError.clear();

        HINTERNET hSession = WinHttpOpen(L"Nebula/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
        {
            outError = L"WinHTTP session failed.";
            return false;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            outError = L"WinHTTP connect failed.";
            return false;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/login/oauth/access_token", nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"WinHTTP request failed.";
            return false;
        }

        std::wstring bodyW =
            L"client_id=" + UrlEncode(clientId) +
            L"&client_secret=" + UrlEncode(clientSecret) +
            L"&code=" + UrlEncode(code) +
            L"&redirect_uri=" + UrlEncode(redirectUri) +
            L"&state=" + UrlEncode(state);
        std::string body = WideToUtf8(bodyW);

        const wchar_t *headers = L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n";
        BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok)
            ok = WinHttpReceiveResponse(hRequest, nullptr);
        if (!ok)
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            outError = L"OAuth session exchange request failed.";
            return false;
        }

        std::string response;
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
            response.append(chunk.data(), chunk.data() + downloaded);
        } while (available > 0);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        std::string key = "\"access_token\":\"";
        size_t pos = response.find(key);
        if (pos == std::string::npos)
        {
            key = "\"access_token\": \"";
            pos = response.find(key);
        }
        if (pos == std::string::npos)
        {
            outError = L"OAuth response has no access credential.";
            return false;
        }
        pos += key.size();
        size_t end = response.find('"', pos);
        if (end == std::string::npos || end <= pos)
        {
            outError = L"Invalid OAuth credential format.";
            return false;
        }

        std::string token = response.substr(pos, end - pos);
        outToken = Utf8ToWide(token);
        if (outToken.empty())
        {
            outError = L"Received empty OAuth credential.";
            return false;
        }
        return true;
    }
}

GitHubOAuth::AuthResult GitHubOAuth::SignInViaBrowser()
{
    AuthResult result;

    std::wstring clientId, clientSecret, redirectUri, err;
    if (!LoadConfig(clientId, clientSecret, redirectUri, err))
    {
        result.success = false;
        result.message = err;
        return result;
    }

    std::wstring state = GenerateState();
    std::wstring authUrl = L"https://github.com/login/oauth/authorize?client_id=" + UrlEncode(clientId) +
                           L"&redirect_uri=" + UrlEncode(redirectUri) +
                           L"&scope=" + UrlEncode(L"repo") +
                           L"&state=" + UrlEncode(state);

    if ((INT_PTR)ShellExecuteW(nullptr, L"open", authUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
    {
        result.success = false;
        result.message = L"Cannot open browser for GitHub OAuth.";
        return result;
    }

    unsigned short callbackPort = 8080;
    std::wstring callbackPath = L"/auth/github/callback";
    size_t scheme = redirectUri.find(L"://");
    if (scheme != std::wstring::npos)
    {
        size_t hostStart = scheme + 3;
        size_t slash = redirectUri.find(L'/', hostStart);
        std::wstring hostPort = (slash == std::wstring::npos) ? redirectUri.substr(hostStart)
                                                               : redirectUri.substr(hostStart, slash - hostStart);
        if (slash != std::wstring::npos)
            callbackPath = redirectUri.substr(slash);
        size_t colon = hostPort.rfind(L':');
        if (colon != std::wstring::npos && colon + 1 < hostPort.size())
        {
            int p = _wtoi(hostPort.substr(colon + 1).c_str());
            if (p > 0 && p <= 65535)
                callbackPort = (unsigned short)p;
        }
    }

    std::wstring code;
    if (!WaitForCallback(callbackPort, callbackPath, state, code, err))
    {
        result.success = false;
        result.message = err;
        return result;
    }

    std::wstring token;
    if (!ExchangeCodeForToken(clientId, clientSecret, code, redirectUri, state, token, err))
    {
        result.success = false;
        result.message = err;
        return result;
    }

    std::wstring saveErr;
    if (!GitHubAuth::SaveToken(token, saveErr))
    {
        result.success = false;
        result.message = saveErr.empty() ? L"Failed to save GitHub session." : saveErr;
        return result;
    }

    if (!token.empty())
        SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
    if (!clientSecret.empty())
        SecureZeroMemory(clientSecret.data(), clientSecret.size() * sizeof(wchar_t));

    result.success = true;
    result.message = L"GitHub connected with OAuth.";
    return result;
}
