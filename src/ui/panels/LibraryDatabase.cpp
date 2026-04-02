#include "LibraryDatabase.h"
#include "core/cmake/CmakeParser.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t kMarketplaceHost[] = L"api.astracode.dev";
    constexpr wchar_t kMarketplaceBasePath[] = L"/marketplace/libraries";
    constexpr int kMarketplacePageSize = 100;

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
            return {};

        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (len <= 0)
            len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (len <= 0)
            return {};

        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
        return out;
    }

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
            return {};

        int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return {};

        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::string UrlEncode(const std::string& value)
    {
        static const char* kHex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size());

        for (unsigned char ch : value)
        {
            const bool unreserved = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
            if (unreserved)
            {
                out.push_back(static_cast<char>(ch));
                continue;
            }

            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0x0F]);
            out.push_back(kHex[ch & 0x0F]);
        }

        return out;
    }

    std::wstring Trim(const std::wstring& text)
    {
        size_t left = 0;
        while (left < text.size() && std::iswspace(text[left]))
            ++left;

        size_t right = text.size();
        while (right > left && std::iswspace(text[right - 1]))
            --right;

        return text.substr(left, right - left);
    }

    std::wstring ToTitleCase(const std::wstring& text)
    {
        if (text.empty())
            return {};

        std::wstring out = text;
        bool upperNext = true;
        for (wchar_t& ch : out)
        {
            if (ch == L'-' || ch == L'_' || ch == L'/')
            {
                ch = L' ';
                upperNext = true;
                continue;
            }

            ch = upperNext
                ? static_cast<wchar_t>(std::towupper(ch))
                : static_cast<wchar_t>(std::towlower(ch));
            upperNext = std::iswspace(ch) != 0;
        }

        return out;
    }

    std::wstring RepoFolderFromGitUrl(const std::wstring& gitUrl)
    {
        if (gitUrl.empty())
            return {};

        std::wstring url = gitUrl;
        while (!url.empty() && (url.back() == L'/' || url.back() == L'\\'))
            url.pop_back();

        if (url.size() >= 4)
        {
            std::wstring tail = url.substr(url.size() - 4);
            std::transform(tail.begin(), tail.end(), tail.begin(), ::towlower);
            if (tail == L".git")
                url.resize(url.size() - 4);
        }

        const size_t pos = url.find_last_of(L"/:\\");
        if (pos != std::wstring::npos && pos + 1 < url.size())
            return url.substr(pos + 1);
        return url;
    }

    std::wstring ToLowerWide(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), ::towlower);
        return value;
    }

    bool StartsWithExternalPath(const std::wstring& relPath)
    {
        std::wstring normalized = relPath;
        std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
        normalized = ToLowerWide(normalized);
        return normalized.rfind(L"external/", 0) == 0;
    }

    bool ParseGitHubOwnerRepo(const std::wstring& gitUrl, std::wstring& owner, std::wstring& repo)
    {
        owner.clear();
        repo.clear();

        const std::wstring prefix = L"https://github.com/";
        if (gitUrl.size() <= prefix.size() || gitUrl.substr(0, prefix.size()) != prefix)
            return false;

        std::wstring rest = gitUrl.substr(prefix.size());
        if (rest.size() > 4 && rest.substr(rest.size() - 4) == L".git")
            rest.resize(rest.size() - 4);

        const size_t slash = rest.find(L'/');
        if (slash == std::wstring::npos)
            return false;

        owner = rest.substr(0, slash);
        repo = rest.substr(slash + 1);
        return !owner.empty() && !repo.empty();
    }

    std::wstring AvatarUrlFromAuthor(const std::wstring& author)
    {
        if (author.empty())
            return {};
        return L"https://github.com/" + author + L".png?size=96";
    }

    bool ParseOwnerRepoFromFullName(const std::wstring& fullName, std::wstring& owner, std::wstring& repo)
    {
        owner.clear();
        repo.clear();

        if (fullName.empty())
            return false;

        const size_t slash = fullName.find(L'/');
        if (slash == std::wstring::npos || slash == 0 || slash + 1 >= fullName.size())
            return false;

        owner = fullName.substr(0, slash);
        repo = fullName.substr(slash + 1);
        return !owner.empty() && !repo.empty();
    }

    bool HttpGetMarketplace(const std::wstring& requestPath, std::string& outBody, std::wstring& outError)
    {
        outBody.clear();
        outError.clear();

        HINTERNET session = WinHttpOpen(
            L"Nebula/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
        {
            outError = L"Unable to start WinHTTP session.";
            return false;
        }

        HINTERNET connection = WinHttpConnect(session, kMarketplaceHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
        {
            WinHttpCloseHandle(session);
            outError = L"Unable to connect to api.astracode.dev.";
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(
            connection,
            L"GET",
            requestPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            outError = L"Unable to create marketplace request.";
            return false;
        }

        const wchar_t* headers = L"Accept: application/json\r\n";
        BOOL ok = WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0);
        if (ok)
            ok = WinHttpReceiveResponse(request, nullptr);

        if (!ok)
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            outError = L"Marketplace request failed.";
            return false;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            statusCode = 0;
        }

        if (statusCode < 200 || statusCode >= 300)
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            outError = L"Marketplace server returned HTTP " + std::to_wstring(static_cast<unsigned long long>(statusCode)) + L".";
            return false;
        }

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0)
        {
            const size_t oldSize = outBody.size();
            outBody.resize(oldSize + available);

            DWORD downloaded = 0;
            if (!WinHttpReadData(request, outBody.data() + oldSize, available, &downloaded))
            {
                outBody.clear();
                break;
            }

            outBody.resize(oldSize + downloaded);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);

        if (outBody.empty())
        {
            outError = L"Marketplace server returned an empty response.";
            return false;
        }

        return true;
    }

    enum class JsonType
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    struct JsonValue
    {
        JsonType type = JsonType::Null;
        bool booleanValue = false;
        double numberValue = 0.0;
        std::string stringValue;
        std::vector<JsonValue> arrayValue;
        std::map<std::string, JsonValue> objectValue;

        const JsonValue* Find(const std::string& key) const
        {
            auto it = objectValue.find(key);
            return it == objectValue.end() ? nullptr : &it->second;
        }
    };

    class JsonParser
    {
    public:
        explicit JsonParser(const std::string& text)
            : text_(text)
        {
        }

        bool Parse(JsonValue& outValue, std::string& outError)
        {
            SkipWhitespace();
            if (!ParseValue(outValue, outError))
                return false;

            SkipWhitespace();
            if (pos_ != text_.size())
            {
                outError = "Unexpected trailing data.";
                return false;
            }

            return true;
        }

    private:
        void SkipWhitespace()
        {
            while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }

        bool ParseValue(JsonValue& outValue, std::string& outError)
        {
            SkipWhitespace();
            if (pos_ >= text_.size())
            {
                outError = "Unexpected end of JSON.";
                return false;
            }

            const char ch = text_[pos_];
            if (ch == '{')
                return ParseObject(outValue, outError);
            if (ch == '[')
                return ParseArray(outValue, outError);
            if (ch == '"')
            {
                outValue.type = JsonType::String;
                return ParseString(outValue.stringValue, outError);
            }
            if (ch == '-' || (ch >= '0' && ch <= '9'))
                return ParseNumber(outValue, outError);
            if (MatchLiteral("true"))
            {
                outValue.type = JsonType::Boolean;
                outValue.booleanValue = true;
                return true;
            }
            if (MatchLiteral("false"))
            {
                outValue.type = JsonType::Boolean;
                outValue.booleanValue = false;
                return true;
            }
            if (MatchLiteral("null"))
            {
                outValue.type = JsonType::Null;
                return true;
            }

            outError = "Unexpected token in JSON.";
            return false;
        }

        bool ParseObject(JsonValue& outValue, std::string& outError)
        {
            ++pos_;
            outValue.type = JsonType::Object;
            outValue.objectValue.clear();

            SkipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == '}')
            {
                ++pos_;
                return true;
            }

            while (pos_ < text_.size())
            {
                std::string key;
                if (!ParseString(key, outError))
                    return false;

                SkipWhitespace();
                if (pos_ >= text_.size() || text_[pos_] != ':')
                {
                    outError = "Expected ':' after object key.";
                    return false;
                }

                ++pos_;
                JsonValue value;
                if (!ParseValue(value, outError))
                    return false;
                outValue.objectValue[key] = std::move(value);

                SkipWhitespace();
                if (pos_ >= text_.size())
                {
                    outError = "Unexpected end of object.";
                    return false;
                }

                if (text_[pos_] == '}')
                {
                    ++pos_;
                    return true;
                }
                if (text_[pos_] != ',')
                {
                    outError = "Expected ',' in object.";
                    return false;
                }

                ++pos_;
                SkipWhitespace();
            }

            outError = "Unexpected end of object.";
            return false;
        }

        bool ParseArray(JsonValue& outValue, std::string& outError)
        {
            ++pos_;
            outValue.type = JsonType::Array;
            outValue.arrayValue.clear();

            SkipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ']')
            {
                ++pos_;
                return true;
            }

            while (pos_ < text_.size())
            {
                JsonValue value;
                if (!ParseValue(value, outError))
                    return false;
                outValue.arrayValue.push_back(std::move(value));

                SkipWhitespace();
                if (pos_ >= text_.size())
                {
                    outError = "Unexpected end of array.";
                    return false;
                }

                if (text_[pos_] == ']')
                {
                    ++pos_;
                    return true;
                }
                if (text_[pos_] != ',')
                {
                    outError = "Expected ',' in array.";
                    return false;
                }

                ++pos_;
                SkipWhitespace();
            }

            outError = "Unexpected end of array.";
            return false;
        }

        bool ParseString(std::string& outString, std::string& outError)
        {
            outString.clear();
            if (pos_ >= text_.size() || text_[pos_] != '"')
            {
                outError = "Expected string.";
                return false;
            }

            ++pos_;
            while (pos_ < text_.size())
            {
                char ch = text_[pos_++];
                if (ch == '"')
                    return true;

                if (ch != '\\')
                {
                    outString.push_back(ch);
                    continue;
                }

                if (pos_ >= text_.size())
                {
                    outError = "Invalid escape sequence.";
                    return false;
                }

                char escaped = text_[pos_++];
                switch (escaped)
                {
                case '"': outString.push_back('"'); break;
                case '\\': outString.push_back('\\'); break;
                case '/': outString.push_back('/'); break;
                case 'b': outString.push_back('\b'); break;
                case 'f': outString.push_back('\f'); break;
                case 'n': outString.push_back('\n'); break;
                case 'r': outString.push_back('\r'); break;
                case 't': outString.push_back('\t'); break;
                case 'u':
                    if (pos_ + 4 <= text_.size())
                    {
                        outString.push_back('?');
                        pos_ += 4;
                    }
                    else
                    {
                        outError = "Invalid unicode escape.";
                        return false;
                    }
                    break;
                default:
                    outError = "Unsupported escape sequence.";
                    return false;
                }
            }

            outError = "Unterminated string.";
            return false;
        }

        bool ParseNumber(JsonValue& outValue, std::string& outError)
        {
            const size_t start = pos_;

            if (text_[pos_] == '-')
                ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
            if (pos_ < text_.size() && text_[pos_] == '.')
            {
                ++pos_;
                while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                    ++pos_;
            }
            if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
            {
                ++pos_;
                if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                    ++pos_;
                while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                    ++pos_;
            }

            try
            {
                outValue.type = JsonType::Number;
                outValue.numberValue = std::stod(text_.substr(start, pos_ - start));
                return true;
            }
            catch (...)
            {
                outError = "Invalid number.";
                return false;
            }
        }

        bool MatchLiteral(const char* literal)
        {
            const size_t len = std::strlen(literal);
            if (text_.compare(pos_, len, literal) != 0)
                return false;
            pos_ += len;
            return true;
        }

        const std::string& text_;
        size_t pos_ = 0;
    };

    std::wstring JsonToWideString(const JsonValue* value)
    {
        if (!value)
            return {};

        switch (value->type)
        {
        case JsonType::String:
            return Utf8ToWide(value->stringValue);
        case JsonType::Number:
            return std::to_wstring(static_cast<long long>(std::llround(value->numberValue)));
        case JsonType::Boolean:
            return value->booleanValue ? L"true" : L"false";
        default:
            return {};
        }
    }

    int JsonToInt(const JsonValue* value, int fallback = 0)
    {
        if (!value)
            return fallback;
        if (value->type == JsonType::Number)
            return static_cast<int>(std::llround(value->numberValue));
        if (value->type == JsonType::String)
        {
            try
            {
                return std::stoi(value->stringValue);
            }
            catch (...)
            {
                return fallback;
            }
        }
        return fallback;
    }

    std::vector<std::wstring> JsonArrayToWideStrings(const JsonValue* value)
    {
        std::vector<std::wstring> out;
        if (!value || value->type != JsonType::Array)
            return out;

        for (const JsonValue& item : value->arrayValue)
        {
            if (item.type == JsonType::String)
                out.push_back(Utf8ToWide(item.stringValue));
        }
        return out;
    }

    void MergeTags(std::vector<std::wstring>& target, const std::vector<std::wstring>& source)
    {
        std::set<std::wstring> seen;
        for (const std::wstring& tag : target)
        {
            std::wstring lowered = tag;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::towlower);
            seen.insert(lowered);
        }

        for (const std::wstring& tag : source)
        {
            std::wstring trimmed = Trim(tag);
            if (trimmed.empty())
                continue;

            std::wstring lowered = trimmed;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::towlower);
            if (seen.insert(lowered).second)
                target.push_back(trimmed);
        }
    }

    bool ParseMarketplaceResponse(const std::string& body, std::vector<LibraryInfo>& outLibraries, std::wstring& outError)
    {
        outLibraries.clear();
        outError.clear();

        JsonParser parser(body);
        JsonValue root;
        std::string parseError;
        if (!parser.Parse(root, parseError))
        {
            outError = L"Unable to parse marketplace response.";
            return false;
        }

        const JsonValue* items = root.Find("items");
        if (!items || items->type != JsonType::Array)
        {
            outError = L"Marketplace response is missing the items array.";
            return false;
        }

        for (const JsonValue& item : items->arrayValue)
        {
            if (item.type != JsonType::Object)
                continue;

            LibraryInfo library;
            library.id = JsonToInt(item.Find("id"));
            library.name = Trim(JsonToWideString(item.Find("name")));
            library.gitUrl = Trim(JsonToWideString(item.Find("github_url")));
            if (library.gitUrl.empty())
                library.gitUrl = Trim(JsonToWideString(item.Find("git_url")));
            library.description = Trim(JsonToWideString(item.Find("description")));
            library.readmeSummary = Trim(JsonToWideString(item.Find("readme_summary")));
            library.language = Trim(JsonToWideString(item.Find("language")));
            library.license = Trim(JsonToWideString(item.Find("license")));
            library.homepage = Trim(JsonToWideString(item.Find("homepage")));
            library.status = Trim(JsonToWideString(item.Find("status")));
            library.stars = JsonToInt(item.Find("stars"));
            library.category = ToTitleCase(Trim(JsonToWideString(item.Find("ai_category"))));

            std::wstring fullName = Trim(JsonToWideString(item.Find("full_name")));
            std::wstring owner;
            std::wstring repo;
            if (library.gitUrl.empty() && ParseOwnerRepoFromFullName(fullName, owner, repo))
                library.gitUrl = L"https://github.com/" + owner + L"/" + repo;

            if (library.description.empty())
                library.description = library.readmeSummary;
            if (library.description.empty())
                library.description = L"No description available.";
            if (library.category.empty())
                library.category = L"Library";

            if (ParseGitHubOwnerRepo(library.gitUrl, owner, repo))
            {
                library.author = owner;
                library.avatarUrl = AvatarUrlFromAuthor(owner);
            }
            else if (ParseOwnerRepoFromFullName(fullName, owner, repo))
            {
                library.author = owner;
                library.avatarUrl = AvatarUrlFromAuthor(owner);
            }

            MergeTags(library.tags, JsonArrayToWideStrings(item.Find("ai_tags")));
            MergeTags(library.tags, JsonArrayToWideStrings(item.Find("topics")));

            if (!library.status.empty())
            {
                std::wstring lowered = library.status;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::towlower);
                if (lowered != L"approved")
                    continue;
            }

            if (library.name.empty())
                continue;

            outLibraries.push_back(std::move(library));
        }

        return true;
    }
}

LibraryDatabase& LibraryDatabase::Instance()
{
    static LibraryDatabase instance;
    return instance;
}

LibraryDatabase::LibraryDatabase() = default;

void LibraryDatabase::BumpRevision()
{
    revision_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<LibraryInfo> LibraryDatabase::GetLibrariesCopy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return libraries_;
}

bool LibraryDatabase::GetLibraryCopy(const std::wstring& name, LibraryInfo& outLibrary) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(libraries_.begin(), libraries_.end(), [&name](const LibraryInfo& library) {
        return _wcsicmp(library.name.c_str(), name.c_str()) == 0;
    });

    if (it == libraries_.end())
        return false;

    outLibrary = *it;
    return true;
}

bool LibraryDatabase::IsInstalled(const std::wstring& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(libraries_.begin(), libraries_.end(), [&name](const LibraryInfo& library) {
        return _wcsicmp(library.name.c_str(), name.c_str()) == 0;
    });
    return it != libraries_.end() && it->isInstalled;
}

void LibraryDatabase::SetInstalled(const std::wstring& name, bool installed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(libraries_.begin(), libraries_.end(), [&name](const LibraryInfo& library) {
        return _wcsicmp(library.name.c_str(), name.c_str()) == 0;
    });

    if (it != libraries_.end() && it->isInstalled != installed)
    {
        it->isInstalled = installed;
        if (installed && it->installState == LibraryInfo::InstallState::Installing)
            it->installState = LibraryInfo::InstallState::Idle;
        BumpRevision();
    }
}

void LibraryDatabase::SetInstallStatus(const std::wstring& name,
                                       LibraryInfo::InstallState state,
                                       const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(libraries_.begin(), libraries_.end(), [&name](const LibraryInfo& library) {
        return _wcsicmp(library.name.c_str(), name.c_str()) == 0;
    });

    if (it == libraries_.end())
        return;

    if (it->installState != state || it->installMessage != message)
    {
        it->installState = state;
        it->installMessage = message;
        BumpRevision();
    }
}

void LibraryDatabase::RefreshInstallationStatus(const std::wstring& projectRoot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring effectiveProjectRoot = projectRoot;
    if (effectiveProjectRoot.empty())
        effectiveProjectRoot = lastProjectRoot_;
    else
        lastProjectRoot_ = effectiveProjectRoot;

    for (LibraryInfo& library : libraries_)
        library.isInstalled = false;

    fs::path externalDir;
    if (!effectiveProjectRoot.empty())
    {
        externalDir = fs::path(effectiveProjectRoot) / L"external";
    }
    else
    {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        fs::path dir = fs::path(exePath).parent_path();
        for (int i = 0; i < 6; ++i)
        {
            if (fs::exists(dir / L"CMakeLists.txt"))
            {
                externalDir = dir / L"external";
                break;
            }

            fs::path parent = dir.parent_path();
            if (parent == dir)
                break;
            dir = parent;
        }
    }

    if (externalDir.empty() || !fs::exists(externalDir) || !fs::is_directory(externalDir))
    {
        BumpRevision();
        return;
    }

    for (LibraryInfo& library : libraries_)
    {
        const std::wstring repoFolder = RepoFolderFromGitUrl(library.gitUrl);
        fs::path libraryPath = repoFolder.empty() ? (externalDir / library.name) : (externalDir / repoFolder);
        bool installed = fs::exists(libraryPath) && fs::is_directory(libraryPath);

        if (!installed)
        {
            fs::path fallback = externalDir / library.name;
            installed = fs::exists(fallback) && fs::is_directory(fallback);
        }

        library.isInstalled = installed;
    }

    // Include project-declared external libraries even if they are not present
    // in the currently fetched marketplace page.
    if (!effectiveProjectRoot.empty())
    {
        fs::path projectCmake = fs::path(effectiveProjectRoot) / L"CMakeLists.txt";
        if (fs::exists(projectCmake) && fs::is_regular_file(projectCmake))
        {
            CmakeProjectInfo projectInfo = ParseCmakeLists(projectCmake.wstring());

            for (const CmakeSubLib& lib : projectInfo.libraries)
            {
                if (!StartsWithExternalPath(lib.subdirRel))
                    continue;

                std::wstring folderName;
                if (!lib.subdirAbs.empty())
                    folderName = fs::path(lib.subdirAbs).filename().wstring();
                if (folderName.empty() && !lib.subdirRel.empty())
                    folderName = fs::path(lib.subdirRel).filename().wstring();
                if (folderName.empty())
                    continue;

                fs::path folderPath = externalDir / folderName;
                if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
                    continue;

                bool found = false;
                for (LibraryInfo& existing : libraries_)
                {
                    std::wstring existingName = ToLowerWide(existing.name);
                    std::wstring repoFolder = ToLowerWide(RepoFolderFromGitUrl(existing.gitUrl));
                    std::wstring folderLower = ToLowerWide(folderName);

                    if ((!existingName.empty() && existingName == folderLower) ||
                        (!repoFolder.empty() && repoFolder == folderLower))
                    {
                        existing.isInstalled = true;
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    LibraryInfo local;
                    local.name = folderName;
                    local.description = L"Installed in this project (local library).";
                    local.category = L"Local";
                    local.status = L"approved";
                    local.isInstalled = true;
                    libraries_.push_back(std::move(local));
                }
            }
        }
    }

    BumpRevision();
}

void LibraryDatabase::RequestLibrariesAsync(const std::wstring& searchQuery, HWND hwnd)
{
    const std::wstring normalizedQuery = Trim(searchQuery);
    unsigned long long requestId = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isLoading_ && normalizedQuery == lastRequestedQuery_)
            return;
        if (normalizedQuery == lastRequestedQuery_ && (hasLoadedOnce_ || !lastError_.empty()))
            return;

        lastRequestedQuery_ = normalizedQuery;
        lastError_.clear();
        isLoading_ = true;
        requestId = ++nextRequestId_;
        BumpRevision();
    }

    if (hwnd)
        InvalidateRect(hwnd, nullptr, FALSE);

    std::thread([this, normalizedQuery, hwnd, requestId]() {
        std::wstring requestPath = std::wstring(kMarketplaceBasePath) +
            L"?sort=stars&limit=" + std::to_wstring(kMarketplacePageSize) +
            L"&offset=0";

        if (!normalizedQuery.empty())
            requestPath += L"&search=" + Utf8ToWide(UrlEncode(WideToUtf8(normalizedQuery)));

        std::string body;
        std::wstring error;
        std::vector<LibraryInfo> fetched;
        bool success = HttpGetMarketplace(requestPath, body, error);
        if (success)
            success = ParseMarketplaceResponse(body, fetched, error);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requestId != nextRequestId_.load())
                return;

            isLoading_ = false;
            if (!success)
            {
                lastError_ = error.empty() ? L"Unable to load marketplace libraries." : error;
                BumpRevision();
            }
            else
            {
                std::map<std::wstring, std::pair<LibraryInfo::InstallState, std::wstring>> installStateByName;
                for (const LibraryInfo &existing : libraries_)
                {
                    std::wstring lowered = existing.name;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::towlower);
                    installStateByName[lowered] = std::make_pair(existing.installState, existing.installMessage);
                }

                libraries_ = std::move(fetched);
                for (LibraryInfo &library : libraries_)
                {
                    std::wstring lowered = library.name;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::towlower);
                    auto it = installStateByName.find(lowered);
                    if (it != installStateByName.end())
                    {
                        library.installState = it->second.first;
                        library.installMessage = it->second.second;
                    }
                }
                lastError_.clear();
                hasLoadedOnce_ = true;
                BumpRevision();
            }
        }

        RefreshInstallationStatus();

        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
    }).detach();
}

bool LibraryDatabase::IsLoading() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return isLoading_;
}

std::wstring LibraryDatabase::GetLastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}
