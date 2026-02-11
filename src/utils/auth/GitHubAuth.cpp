#include "utils/auth/GitHubAuth.h"

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

namespace
{
    std::filesystem::path GetStorePath()
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
            out /= L"github.token";
        }
        return out;
    }

    bool WideToUtf8(const std::wstring &in, std::string &out)
    {
        out.clear();
        if (in.empty())
            return true;
        int len = WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return false;
        out.resize((size_t)len);
        return WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(), out.data(), len, nullptr, nullptr) > 0;
    }

    bool Utf8ToWide(const std::string &in, std::wstring &out)
    {
        out.clear();
        if (in.empty())
            return true;
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), (int)in.size(), nullptr, 0);
        if (len <= 0)
            return false;
        out.resize((size_t)len);
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), (int)in.size(), out.data(), len) > 0;
    }
}

bool GitHubAuth::SaveToken(const std::wstring &token, std::wstring &outError)
{
    outError.clear();

    if (token.empty())
    {
        outError = L"Session GitHub vide.";
        return false;
    }

    std::string utf8Token;
    if (!WideToUtf8(token, utf8Token))
    {
        outError = L"Impossible d'encoder la session GitHub.";
        return false;
    }

    DATA_BLOB inputBlob = {};
    inputBlob.pbData = (BYTE *)utf8Token.data();
    inputBlob.cbData = (DWORD)utf8Token.size();

    DATA_BLOB outputBlob = {};
    if (!CryptProtectData(&inputBlob, L"NebulaGitHubToken", nullptr, nullptr, nullptr, 0, &outputBlob))
    {
        outError = L"Impossible de chiffrer la session GitHub.";
        SecureZeroMemory(utf8Token.data(), utf8Token.size());
        return false;
    }

    std::filesystem::path path = GetStorePath();
    if (path.empty())
    {
        outError = L"Impossible d'acceder au dossier AppData.";
        SecureZeroMemory(utf8Token.data(), utf8Token.size());
        if (outputBlob.pbData)
            LocalFree(outputBlob.pbData);
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        outError = L"Impossible d'ecrire la session GitHub.";
        SecureZeroMemory(utf8Token.data(), utf8Token.size());
        if (outputBlob.pbData)
            LocalFree(outputBlob.pbData);
        return false;
    }

    out.write((const char *)outputBlob.pbData, (std::streamsize)outputBlob.cbData);
    out.close();

    SecureZeroMemory(utf8Token.data(), utf8Token.size());
    if (outputBlob.pbData)
    {
        SecureZeroMemory(outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
    }

    if (!out)
    {
        outError = L"Echec d'ecriture de la session GitHub.";
        return false;
    }
    return true;
}

bool GitHubAuth::LoadToken(std::wstring &outToken, std::wstring &outError)
{
    outToken.clear();
    outError.clear();

    std::filesystem::path path = GetStorePath();
    if (path.empty() || !std::filesystem::exists(path))
    {
        outError = L"GitHub non connecte.";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        outError = L"Impossible de lire la session GitHub.";
        return false;
    }

    std::vector<BYTE> encrypted((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (encrypted.empty())
    {
        outError = L"Session GitHub invalide.";
        return false;
    }

    DATA_BLOB inputBlob = {};
    inputBlob.pbData = encrypted.data();
    inputBlob.cbData = (DWORD)encrypted.size();

    DATA_BLOB outputBlob = {};
    if (!CryptUnprotectData(&inputBlob, nullptr, nullptr, nullptr, nullptr, 0, &outputBlob))
    {
        outError = L"Impossible de dechiffrer la session GitHub.";
        SecureZeroMemory(encrypted.data(), encrypted.size());
        return false;
    }

    std::string utf8Token((const char *)outputBlob.pbData, (size_t)outputBlob.cbData);
    bool ok = Utf8ToWide(utf8Token, outToken);

    SecureZeroMemory(encrypted.data(), encrypted.size());
    SecureZeroMemory(utf8Token.data(), utf8Token.size());
    if (outputBlob.pbData)
    {
        SecureZeroMemory(outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
    }

    if (!ok || outToken.empty())
    {
        outError = L"Session GitHub invalide.";
        outToken.clear();
        return false;
    }
    return true;
}

bool GitHubAuth::HasToken()
{
    std::wstring token;
    std::wstring err;
    const bool ok = LoadToken(token, err);
    if (!token.empty())
        SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
    return ok;
}

bool GitHubAuth::ClearToken(std::wstring &outError)
{
    outError.clear();
    std::filesystem::path path = GetStorePath();
    if (path.empty())
    {
        outError = L"Impossible d'acceder au dossier AppData.";
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        std::filesystem::remove(path, ec);
    if (ec)
    {
        outError = L"Impossible de supprimer la session GitHub.";
        return false;
    }
    return true;
}
