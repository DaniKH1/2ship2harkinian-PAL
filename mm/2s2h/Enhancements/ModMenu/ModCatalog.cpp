#include "ModCatalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>
#include <cwctype>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>
#include <zip.h>

#include <nlohmann/json.hpp>

#include <SDL2/SDL.h>
#include <fast/Fast3dGui.h>
#include <fast/Fast3dWindow.h>
#include <libultraship/libultraship.h>
#include <webp/decode.h>

namespace ModCatalog {
namespace {
using Json = nlohmann::json;

enum class Source {
    GameBanana,
    ModLoaderModels,
    ModLoaderPaks,
    TexturePacks,
};

struct DownloadOption {
    std::string label;
    std::string url;
    std::string filename;
};

struct Entry {
    Source source;
    std::string id;
    std::string name;
    std::string author;
    std::string downloadUrl;
    std::string profileUrl;
    std::string imageUrl;
    std::filesystem::path imagePath;
    bool downloadable = true;
    std::string downloadFilename;
    std::vector<DownloadOption> downloadOptions;
};

struct SearchResult {
    std::vector<Entry> entries;
    std::string error;
};

struct DownloadResult {
    bool installed = false;
    std::string error;
};

struct DownloadProgress {
    std::atomic<uint64_t> bytesReceived = 0;
    std::atomic<uint64_t> totalBytes = 0;
    std::atomic<bool> requestStarted = false;
    std::atomic<bool> transferComplete = false;
};

struct TabState {
    std::array<char, 128> query = {};
    std::string modelCategory = "child";
    std::string lastQuery;
    std::string lastModelCategory;
    std::chrono::steady_clock::time_point changedAt = std::chrono::steady_clock::now();
    std::vector<Entry> entries;
    std::string error;
    std::future<SearchResult> search;
    std::string runningQuery;
    std::string runningModelCategory;
    bool hasSearched = false;
    std::future<std::vector<Entry>> thumbnails;
    std::vector<Entry> pendingThumbnailEntries;
    bool hasPendingThumbnailLoad = false;
    std::future<DownloadResult> download;
    std::string downloadingId;
    std::shared_ptr<DownloadProgress> downloadProgress;
    std::string status;
    std::unordered_set<std::string> failedImages;
};

static TabState gameBananaState;
static TabState modelState;
static TabState pakState;
static TabState texturePackState;

class InternetHandle {
  public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle(handle) {}
    ~InternetHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET Get() const {
        return handle;
    }

  private:
    HINTERNET handle;
};

static std::wstring ToWide(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), size);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

static std::filesystem::path PathFromUtf8(const std::string& value) {
    return std::filesystem::path(ToWide(value));
}

static std::string ToUtf8(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

static bool IsAllowedHost(const std::wstring& host) {
    std::wstring normalized = host;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    static const std::array<std::wstring, 5> domains = {
        L"gamebanana.com",
        L"modloader64.com",
        L"github.com",
        L"githubusercontent.com",
        L"emulationking.com",
    };
    for (const auto& domain : domains) {
        if (normalized == domain ||
            (normalized.size() > domain.size() && normalized.compare(normalized.size() - domain.size(), domain.size(),
                                                                     domain) == 0 &&
             normalized[normalized.size() - domain.size() - 1] == L'.')) {
            return true;
        }
    }
    return false;
}

static bool IsAllowedHttpsUrl(const std::string& url) {
    const std::wstring wideUrl = ToWide(url);
    if (wideUrl.empty()) {
        return false;
    }
    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    std::array<wchar_t, 256> hostBuffer = {};
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    return WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components) &&
           components.nScheme == INTERNET_SCHEME_HTTPS &&
           IsAllowedHost(std::wstring(components.lpszHostName, components.dwHostNameLength));
}

static bool HttpGet(std::string url, size_t maxBytes, std::vector<uint8_t>& body, std::string& error) {
    for (int redirectCount = 0; redirectCount <= 5; redirectCount++) {
        const std::wstring wideUrl = ToWide(url);
        if (wideUrl.empty()) {
            error = "The catalog returned an invalid URL.";
            return false;
        }

        URL_COMPONENTS components = {};
        components.dwStructSize = sizeof(components);
        std::array<wchar_t, 256> hostBuffer = {};
        std::array<wchar_t, 8192> pathBuffer = {};
        std::array<wchar_t, 4096> extraBuffer = {};
        components.lpszHostName = hostBuffer.data();
        components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
        components.lpszUrlPath = pathBuffer.data();
        components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());
        components.lpszExtraInfo = extraBuffer.data();
        components.dwExtraInfoLength = static_cast<DWORD>(extraBuffer.size());
        if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components) ||
            components.nScheme != INTERNET_SCHEME_HTTPS) {
            error = "Only HTTPS catalog URLs are allowed.";
            return false;
        }

        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        if (!IsAllowedHost(host)) {
            error = "The catalog returned an unsupported download host.";
            return false;
        }

        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0) {
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        }
        InternetHandle session(WinHttpOpen(L"2Ship2Harkinian Mod Catalog/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.Get() == nullptr) {
            error = "Could not initialize the HTTPS connection.";
            return false;
        }
        if (!WinHttpSetTimeouts(session.Get(), 10000, 10000, 10000, 30000)) {
            error = "Could not configure HTTPS request timeouts.";
            return false;
        }
        InternetHandle connection(WinHttpConnect(session.Get(), host.c_str(), components.nPort, 0));
        if (connection.Get() == nullptr) {
            error = "Could not connect to the mod catalog.";
            return false;
        }
        InternetHandle request(
            WinHttpOpenRequest(connection.Get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (request.Get() == nullptr) {
            error = "Could not create the catalog request.";
            return false;
        }

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy))) {
            error = "Could not enforce the catalog redirect policy.";
            return false;
        }
        if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.Get(), nullptr)) {
            error = "The HTTPS request failed (Windows error " + std::to_string(GetLastError()) + ").";
            return false;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            error = "The catalog returned an unreadable response.";
            return false;
        }
        if (statusCode >= 300 && statusCode < 400) {
            DWORD locationSize = 0;
            WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                                &locationSize, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || locationSize < sizeof(wchar_t)) {
                error = "The catalog returned an invalid redirect.";
                return false;
            }
            std::wstring location(locationSize / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location.data(), &locationSize, WINHTTP_NO_HEADER_INDEX)) {
                error = "Could not read the catalog redirect.";
                return false;
            }
            location.resize(wcsnlen(location.c_str(), location.size()));
            if (location.rfind(L"//", 0) == 0) {
                location = L"https:" + location;
            } else if (location.rfind(L"https://", 0) != 0) {
                if (!location.empty() && location.front() == L'/') {
                    location = L"https://" + host + location;
                } else {
                    error = "The catalog returned an unsupported redirect.";
                    return false;
                }
            }
            url = ToUtf8(location);
            continue;
        }
        if (statusCode != 200) {
            error = "The catalog request failed with HTTP status " + std::to_string(statusCode) + ".";
            return false;
        }

        body.clear();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.Get(), &available)) {
                error = "Could not read the catalog response.";
                return false;
            }
            if (available == 0) {
                return true;
            }
            if (body.size() + available > maxBytes) {
                error = "The catalog response exceeds the allowed size.";
                return false;
            }
            const size_t offset = body.size();
            body.resize(offset + available);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.Get(), body.data() + offset, available, &bytesRead)) {
                error = "Could not read the catalog response.";
                return false;
            }
            body.resize(offset + bytesRead);
        }
    }
    error = "The catalog exceeded the redirect limit.";
    return false;
}

static bool HttpDownloadToFile(std::string url, const std::filesystem::path& destination, uint64_t maxBytes,
                               std::string& error, DownloadProgress* progress) {
    for (int redirectCount = 0; redirectCount <= 5; redirectCount++) {
        const std::wstring wideUrl = ToWide(url);
        URL_COMPONENTS components = {};
        components.dwStructSize = sizeof(components);
        std::array<wchar_t, 256> hostBuffer = {};
        std::array<wchar_t, 8192> pathBuffer = {};
        std::array<wchar_t, 4096> extraBuffer = {};
        components.lpszHostName = hostBuffer.data();
        components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
        components.lpszUrlPath = pathBuffer.data();
        components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());
        components.lpszExtraInfo = extraBuffer.data();
        components.dwExtraInfoLength = static_cast<DWORD>(extraBuffer.size());
        if (wideUrl.empty() ||
            !WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components) ||
            components.nScheme != INTERNET_SCHEME_HTTPS) {
            error = "Only valid HTTPS download URLs are allowed.";
            return false;
        }
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        if (!IsAllowedHost(host)) {
            error = "The download URL uses an unsupported host.";
            return false;
        }
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0) {
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        }

        InternetHandle session(WinHttpOpen(L"2Ship2Harkinian Mod Catalog/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.Get() == nullptr || !WinHttpSetTimeouts(session.Get(), 10000, 10000, 10000, 60000)) {
            error = "Could not initialize the download.";
            return false;
        }
        InternetHandle connection(WinHttpConnect(session.Get(), host.c_str(), components.nPort, 0));
        if (connection.Get() == nullptr) {
            error = "Could not connect to the download host.";
            return false;
        }
        InternetHandle request(
            WinHttpOpenRequest(connection.Get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (request.Get() == nullptr ||
            !WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)) ||
            !WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.Get(), nullptr)) {
            error = "The download request failed (Windows error " + std::to_string(GetLastError()) +
                    ").";
            return false;
        }
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            error = "The download host returned an unreadable response.";
            return false;
        }
        if (statusCode >= 300 && statusCode < 400) {
            DWORD locationSize = 0;
            WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                                &locationSize, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || locationSize < sizeof(wchar_t)) {
                error = "The download host returned an invalid redirect.";
                return false;
            }
            std::wstring location(locationSize / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location.data(), &locationSize, WINHTTP_NO_HEADER_INDEX)) {
                error = "Could not read the download redirect.";
                return false;
            }
            location.resize(wcsnlen(location.c_str(), location.size()));
            if (location.rfind(L"//", 0) == 0) {
                location = L"https:" + location;
            } else if (location.rfind(L"https://", 0) != 0) {
                if (!location.empty() && location.front() == L'/') {
                    location = L"https://" + host + location;
                } else {
                    error = "The download host returned an unsupported redirect.";
                    return false;
                }
            }
            url = ToUtf8(location);
            continue;
        }
        if (statusCode != 200) {
            error = "The download failed with HTTP status " + std::to_string(statusCode) + ".";
            return false;
        }
        if (progress != nullptr) {
            progress->requestStarted.store(true, std::memory_order_relaxed);
        }
        ULONGLONG contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                                WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize,
                                WINHTTP_NO_HEADER_INDEX) &&
            contentLength > maxBytes) {
            error = "The download exceeds its size limit.";
            return false;
        }
        if (progress != nullptr) {
            progress->totalBytes.store(contentLength, std::memory_order_relaxed);
        }

        std::ofstream output(destination, std::ios::binary);
        if (!output) {
            error = "Could not create the downloaded archive.";
            return false;
        }
        std::array<char, 65536> buffer = {};
        uint64_t totalBytes = 0;
        for (;;) {
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
                error = "Could not read the download response.";
                break;
            }
            if (bytesRead == 0) {
                output.close();
                if (output && totalBytes > 0) {
                    return true;
                }
                error = "The download was empty or could not be written.";
                break;
            }
            if (totalBytes + bytesRead > maxBytes) {
                error = "The download exceeds its size limit.";
                break;
            }
            output.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
            if (!output) {
                error = "Could not write the downloaded archive.";
                break;
            }
            totalBytes += bytesRead;
            if (progress != nullptr) {
                progress->bytesReceived.fetch_add(bytesRead, std::memory_order_relaxed);
            }
        }
        output.close();
        std::error_code filesystemError;
        std::filesystem::remove(destination, filesystemError);
        return false;
    }
    error = "The download exceeded the redirect limit.";
    return false;
}

static std::string PercentEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0F]);
        }
    }
    return encoded;
}

static std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

static bool IsOptionalFolderName(std::string name) {
    name = Lowercase(std::move(name));
    return name.find("optional") != std::string::npos || name.find("opcional") != std::string::npos ||
           name.find("bonus") != std::string::npos || name.find("additional") != std::string::npos ||
           name.find("alternate") != std::string::npos || name.find("add-on") != std::string::npos ||
           name.find("addon") != std::string::npos || name == "dlc" || name == "extra" || name == "extras" ||
           name.rfind("extra ", 0) == 0 || name.rfind("extra_", 0) == 0 || name.rfind("extra-", 0) == 0;
}

static std::string OptionalFolderFromArchivePath(const std::filesystem::path& path) {
    std::vector<std::string> folders;
    for (const auto& component : path.parent_path()) {
        folders.push_back(ToUtf8(component.wstring()));
    }
    for (size_t i = 0; i < folders.size(); i++) {
        const std::string& folder = folders[i];
        if (!IsOptionalFolderName(folder)) {
            continue;
        }
        const std::string normalized = Lowercase(folder);
        if (i + 1 < folders.size() &&
            (normalized == "_optional" || normalized == "optional" || normalized == "optionals" ||
             normalized == "opcional" || normalized == "opcionales")) {
            return folders[i + 1];
        }
        return folder;
    }
    return {};
}

static std::string GetImageUrl(const Json& record) {
    if (!record.contains("_aPreviewMedia") || !record["_aPreviewMedia"].contains("_aImages") ||
        !record["_aPreviewMedia"]["_aImages"].is_array() || record["_aPreviewMedia"]["_aImages"].empty()) {
        return {};
    }
    const Json& image = record["_aPreviewMedia"]["_aImages"].front();
    if (!image.contains("_sBaseUrl") || !image.contains("_sFile220")) {
        return {};
    }
    return image["_sBaseUrl"].get<std::string>() + "/" + image["_sFile220"].get<std::string>();
}

static void AppendLittleEndian(std::vector<uint8_t>& output, uint32_t value, size_t byteCount) {
    for (size_t byte = 0; byte < byteCount; byte++) {
        output.push_back(static_cast<uint8_t>(value >> (byte * 8)));
    }
}

static bool SaveWebpThumbnailAsBmp(const std::filesystem::path& path, const std::vector<uint8_t>& imageData) {
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(imageData.data(), imageData.size(), &width, &height) || width <= 0 || height <= 0 ||
        width > 2048 || height > 2048 || static_cast<uint64_t>(width) * height > 4 * 1024 * 1024) {
        return false;
    }

    std::unique_ptr<uint8_t, decltype(&WebPFree)> rgba(
        WebPDecodeRGBA(imageData.data(), imageData.size(), &width, &height), WebPFree);
    if (!rgba) {
        return false;
    }

    const uint32_t pixelDataSize = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4;
    std::vector<uint8_t> bmp;
    bmp.reserve(54 + pixelDataSize);
    AppendLittleEndian(bmp, 0x4D42, 2);
    AppendLittleEndian(bmp, 54 + pixelDataSize, 4);
    AppendLittleEndian(bmp, 0, 4);
    AppendLittleEndian(bmp, 54, 4);
    AppendLittleEndian(bmp, 40, 4);
    AppendLittleEndian(bmp, static_cast<uint32_t>(width), 4);
    AppendLittleEndian(bmp, static_cast<uint32_t>(height), 4);
    AppendLittleEndian(bmp, 1, 2);
    AppendLittleEndian(bmp, 32, 2);
    AppendLittleEndian(bmp, 0, 4);
    AppendLittleEndian(bmp, pixelDataSize, 4);
    AppendLittleEndian(bmp, 2835, 4);
    AppendLittleEndian(bmp, 2835, 4);
    AppendLittleEndian(bmp, 0, 4);
    AppendLittleEndian(bmp, 0, 4);

    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            const uint8_t* pixel = rgba.get() + (static_cast<size_t>(y) * width + x) * 4;
            bmp.push_back(pixel[2]);
            bmp.push_back(pixel[1]);
            bmp.push_back(pixel[0]);
            bmp.push_back(pixel[3]);
        }
    }

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
    return output.good();
}

static void CacheThumbnails(std::vector<Entry>& entries, const std::filesystem::path& modsDirectory) {
    if (modsDirectory.empty()) {
        return;
    }
    const auto cacheDirectory = modsDirectory / ".mod-browser-cache";
    std::error_code filesystemError;
    std::filesystem::create_directories(cacheDirectory, filesystemError);
    if (filesystemError) {
        return;
    }
    size_t downloadedCount = 0;
    for (auto& entry : entries) {
        if (entry.imageUrl.empty() || downloadedCount >= 12) {
            continue;
        }
        const std::string extension = Lowercase(std::filesystem::path(entry.imageUrl).extension().string());
        const bool isWebp = extension == ".webp";
        if (extension != ".jpg" && extension != ".jpeg" && extension != ".png" && !isWebp) {
            continue;
        }
        entry.imagePath = cacheDirectory / (entry.id + (isWebp ? ".bmp" : extension));
        if (std::filesystem::exists(entry.imagePath)) {
            continue;
        }
        std::vector<uint8_t> imageData;
        std::string error;
        if (HttpGet(entry.imageUrl, isWebp ? 4 * 1024 * 1024 : 1024 * 1024, imageData, error)) {
            if (isWebp) {
                if (SaveWebpThumbnailAsBmp(entry.imagePath, imageData)) {
                    downloadedCount++;
                    continue;
                }
                entry.imagePath.clear();
                continue;
            }
            std::ofstream image(entry.imagePath, std::ios::binary);
            if (image && !imageData.empty()) {
                image.write(reinterpret_cast<const char*>(imageData.data()), static_cast<std::streamsize>(imageData.size()));
                if (image) {
                    downloadedCount++;
                    continue;
                }
            }
        }
        entry.imagePath.clear();
    }
}

static SearchResult SearchGameBanana(const std::string& query) {
    SearchResult result;
    const std::string searchQuery = query.empty() ? "Link" : query;
    const std::string url = "https://gamebanana.com/apiv11/Util/Search/Results?_sSearchString=" +
                            PercentEncode(searchQuery) + "&_sModelName=Mod&_idGameRow=20371";
    std::vector<uint8_t> response;
    if (!HttpGet(url, 4 * 1024 * 1024, response, result.error)) {
        return result;
    }
    try {
        const Json data = Json::parse(response.begin(), response.end());
        if (!data.contains("_aRecords") || !data["_aRecords"].is_array()) {
            result.error = "GameBanana returned an unexpected search response.";
            return result;
        }
        std::unordered_set<std::string> seenIds;
        std::unordered_set<std::string> seenNames;
        for (const auto& record : data["_aRecords"]) {
            if (!record.contains("_idRow") || !record.contains("_sName")) {
                continue;
            }
            if (record.value("_bHasFiles", false) == false) {
                continue;
            }
            const std::string id = std::to_string(record["_idRow"].get<uint64_t>());
            const std::string name = record["_sName"].get<std::string>();
            std::string normalizedName = Lowercase(name);
            if (!seenIds.insert(id).second || !seenNames.insert(normalizedName).second) {
                continue;
            }
            Entry entry = { Source::GameBanana,
                            id,
                            name,
                            record.contains("_aSubmitter") ? record["_aSubmitter"].value("_sName", "") : "",
                            {},
                            "https://gamebanana.com/mods/" + std::to_string(record["_idRow"].get<uint64_t>()),
                            GetImageUrl(record),
                            {} };
            result.entries.push_back(std::move(entry));
        }
    } catch (const Json::exception& exception) {
        result.error = std::string("Could not parse GameBanana results: ") + exception.what();
        return result;
    }
    return result;
}

static SearchResult SearchModLoaderModels(const std::string& query, const std::string& category) {
    SearchResult result;
    std::vector<uint8_t> response;
    if (!HttpGet("https://modloader64.com/assets/data/models.json", 8 * 1024 * 1024, response, result.error)) {
        return result;
    }
    try {
        const Json data = Json::parse(response.begin(), response.end());
        if (!data.contains("models") || !data["models"].is_array()) {
            result.error = "ModLoader64 returned an unexpected model catalog.";
            return result;
        }
        const std::string normalizedQuery = Lowercase(query);
        std::unordered_set<std::string> seenNames;
        std::unordered_set<std::string> seenDownloads;
        for (const auto& model : data["models"]) {
            if (!model.contains("games") || !model["games"].is_array() ||
                std::find(model["games"].begin(), model["games"].end(), "mm") == model["games"].end() ||
                !model.contains("category") || model.value("category", "") != category ||
                !model.contains("download") || !model.contains("name")) {
                continue;
            }
            const std::string name = model["name"].get<std::string>();
            const std::string downloadUrl = model["download"].get<std::string>();
            std::string author;
            if (model.contains("authors") && model["authors"].is_array() && !model["authors"].empty()) {
                author = model["authors"].front().is_string() ? model["authors"].front().get<std::string>()
                                                               : model["authors"].front().value("name", "");
            }
            if (Lowercase(name).find(normalizedQuery) == std::string::npos &&
                Lowercase(author).find(normalizedQuery) == std::string::npos) {
                continue;
            }
            if (!seenNames.insert(Lowercase(name)).second ||
                !seenDownloads.insert(Lowercase(downloadUrl)).second) {
                continue;
            }
            std::string imageUrl = model.value("image", "");
            if (!imageUrl.empty() && imageUrl.find("https://") != 0) {
                imageUrl = "https://modloader64.com/" + imageUrl;
            }
            result.entries.push_back({ Source::ModLoaderModels, name, name, author,
                                       downloadUrl, {}, imageUrl, {} });
        }
    } catch (const Json::exception& exception) {
        result.error = std::string("Could not parse ModLoader64 models: ") + exception.what();
        return result;
    }
    return result;
}

static std::string StripHtml(const std::string& value) {
    std::string result;
    bool insideTag = false;
    for (char character : value) {
        if (character == '<') {
            insideTag = true;
        } else if (character == '>') {
            insideTag = false;
        } else if (!insideTag) {
            result.push_back(character);
        }
    }
    return result;
}

static SearchResult SearchModLoaderPaks(const std::string& query) {
    SearchResult result;
    std::vector<uint8_t> response;
    if (!HttpGet("https://modloader64.com/mods.html", 2 * 1024 * 1024, response, result.error)) {
        return result;
    }
    const std::string html(response.begin(), response.end());
    const std::string normalizedQuery = Lowercase(query);
    const std::regex rowExpression(R"(<tr[^>]*onclick="document\.location\s*=\s*'([^']+)';"[^>]*>([\s\S]*?)</tr>)",
                                   std::regex::icase);
    const std::regex cellExpression(R"(<td[^>]*>([\s\S]*?)</td>)", std::regex::icase);
    for (std::sregex_iterator row(html.begin(), html.end(), rowExpression), end; row != end; ++row) {
        const std::string link = (*row)[1].str();
        const std::string rowHtml = (*row)[2].str();
        std::vector<std::string> cells;
        for (std::sregex_iterator cell(rowHtml.begin(), rowHtml.end(), cellExpression), cellEnd; cell != cellEnd;
             ++cell) {
            cells.push_back(StripHtml((*cell)[1].str()));
        }
        if (cells.size() < 3 ||
            (!normalizedQuery.empty() &&
             Lowercase(cells[0] + " " + cells[1] + " " + cells[2]).find(normalizedQuery) == std::string::npos)) {
            continue;
        }
        std::string downloadUrl = link;
        if (downloadUrl.rfind("https://", 0) != 0) {
            if (downloadUrl.empty() || downloadUrl.front() != '/') {
                downloadUrl = "https://modloader64.com/" + downloadUrl;
            } else {
                downloadUrl = "https://modloader64.com" + downloadUrl;
            }
        }
        const bool downloadable = Lowercase(std::filesystem::path(downloadUrl).extension().string()) == ".pak";
        result.entries.push_back({ Source::ModLoaderPaks, std::to_string(result.entries.size()), cells[0], cells[1],
                                   downloadable ? downloadUrl : "", downloadable ? "" : downloadUrl, {}, {},
                                   downloadable });
        if (result.entries.size() >= 30) {
            break;
        }
    }
    return result;
}

static SearchResult SearchTexturePacks(const std::string& query) {
    SearchResult result;
    const std::string catalogRoot = "https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/";
    const std::string downloadRoot = "https://files.emulationking.com/n64/texturepacks/majorasmask/";
    const std::string normalizedQuery = Lowercase(query);

    struct Pack {
        const char* id;
        const char* name;
        const char* author;
        const char* slug;
        const char* image;
        std::vector<DownloadOption> downloads;
    };
    const std::vector<Pack> packs = {
        { "bluedart", "Bluedart's high-res Majora's Mask Texture Pack", "Bluedart, OoT Community, Djipi",
          "bluedart", "Bluedarts-Majoras-Mask-Texture-Pack-Cover-Pic.jpg",
          { { "Rice", "Majoras-Mask-Bluedart.zip", "Majoras-Mask-Bluedart.zip" } } },
        { "djipi20163ds", "Djipi's 2016 3DS Style Majora's Mask Texture Pack", "Djipi", "djipi20163ds",
          "Djipis-3DS-Style-Majoras-Mask-cover-pic.jpg",
          { { "Rice", "Djipis-Majoras-Mask-3DS-2016-Rice.zip", "Djipis-Majoras-Mask-3DS-2016-Rice.zip" },
            { "Glide64 Cache", "Djipis-Majoras-Mask-3DS-2016-Glide64%28DAT%29.zip",
              "Djipis-Majoras-Mask-3DS-2016-Glide64-DAT.zip" },
            { "GlideN64 Cache", "Djipis-Majoras-Mask-3DS-2016-GlideN64%28HTC%29.zip",
              "Djipis-Majoras-Mask-3DS-2016-GlideN64-HTC.zip" } } },
        { "djipi2016celda", "Djipi's 2016 Celda Majora's Mask Texture Pack", "Djipi", "djipi2016celda",
          "Djipi-2016-Celda-Majoras-Mask-Texture-Pack-Cover-Pic.jpg",
          { { "Rice", "Djipis-Majoras-Mask-Celda-2016-Rice.zip", "Djipis-Majoras-Mask-Celda-2016-Rice.zip" },
            { "Glide64 Cache", "Djipis-Majoras-Mask-Celda-2016-Glide64%28DAT%29.zip",
              "Djipis-Majoras-Mask-Celda-2016-Glide64-DAT.zip" },
            { "GlideN64 Cache", "Djipis-Majoras-Mask-Celda-2016-GlideN64%28HTC%29.zip",
              "Djipis-Majoras-Mask-Celda-2016-GlideN64-HTC.zip" } } },
        { "djipi2011celda", "Djipi's Cel Shade 2011 Majora's Mask Texture Pack", "Djipi", "djipi2011celda",
          "Djipis-2011-Majoras-Mask-Texture-Pack-Cover-Pic.jpg",
          { { "Rice", "Djipis-Majoras-Mask-Celda-2011-Rice.zip", "Djipis-Majoras-Mask-Celda-2011-Rice.zip" },
            { "Glide64 Cache", "Djipis-Majoras-Mask-Celda-2011-Glide64%28DAT%29.zip",
              "Djipis-Majoras-Mask-Celda-2011-Glide64-DAT.zip" } } },
        { "djipicellv1", "Djipi's Cell Shade V1 Majora's Mask Texture Pack", "Djipi", "djipicellv1",
          "Djipis-Cel-Shade-v1-Majoras-Mask-Texture-Pack-Cover-Pic.jpg",
          { { "Rice", "Djipis-Majoras-Mask-Celda-2009-Rice.zip", "Djipis-Majoras-Mask-Celda-2009-Rice.zip" } } },
        { "djipirealistic", "Djipi's Realistic Majora's Mask Texture Pack", "Djipi", "djipirealistic",
          "Djipi-Realistic-Majoras-Mask-Texture-Pack-Cover-Pic.jpg",
          { { "Rice", "Djipis-Majoras-Mask-3ds-2009-Rice.zip", "Djipis-Majoras-Mask-3ds-2009-Rice.zip" } } },
    };

    for (const auto& pack : packs) {
        if (!normalizedQuery.empty() && Lowercase(pack.name).find(normalizedQuery) == std::string::npos &&
            Lowercase(pack.author).find(normalizedQuery) == std::string::npos) {
            continue;
        }
        Entry entry = { Source::TexturePacks,
                        pack.id,
                        pack.name,
                        pack.author,
                        {},
                        catalogRoot + pack.slug + "/",
                        catalogRoot + pack.slug + "/" + pack.image,
                        {} };
        for (const auto& option : pack.downloads) {
            DownloadOption download = option;
            download.url = downloadRoot + download.url;
            entry.downloadOptions.push_back(std::move(download));
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

static bool IsModArchive(const std::filesystem::path& path) {
    const std::string extension = Lowercase(path.extension().string());
    return extension == ".o2r" || extension == ".pak";
}

static void RemoveStagingDirectory(const std::filesystem::path& path, std::string& error) {
    std::error_code filesystemError;
    std::filesystem::remove_all(path, filesystemError);
    if (filesystemError) {
        if (!error.empty()) {
            error += " ";
        }
        error += "Temporary download files remain at " + path.string() + ": " + filesystemError.message();
    }
}

static std::string SafeName(std::string name) {
    for (char& character : name) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value < 32 || character == '<' || character == '>' || character == ':' || character == '"' ||
            character == '/' || character == '\\' || character == '|' || character == '?' || character == '*') {
            character = '_';
        }
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
        name.pop_back();
    }
    if (name.empty()) {
        name = "Downloaded Mod";
    }
    if (name.size() > 80) {
        name.resize(80);
    }
    const std::string deviceName = Lowercase(name.substr(0, name.find('.')));
    const bool numberedDevice = deviceName.size() == 4 &&
                                (deviceName.rfind("com", 0) == 0 || deviceName.rfind("lpt", 0) == 0) &&
                                deviceName[3] >= '1' && deviceName[3] <= '9';
    if (deviceName == "con" || deviceName == "prn" || deviceName == "aux" || deviceName == "nul" ||
        numberedDevice) {
        name.insert(name.begin(), '_');
    }
    return name;
}

static std::string GetGameBananaFileUrl(const std::string& modId, std::string& filename, std::string& error) {
    std::vector<uint8_t> response;
    if (!HttpGet("https://gamebanana.com/apiv11/Mod/" + modId + "?_csvProperties=_aFiles", 2 * 1024 * 1024, response,
                 error)) {
        return {};
    }
    try {
        const Json data = Json::parse(response.begin(), response.end());
        if (!data.contains("_aFiles") || !data["_aFiles"].is_array()) {
            error = "GameBanana did not return downloadable files for this mod.";
            return {};
        }
        for (const auto& file : data["_aFiles"]) {
            if (!file.contains("_sDownloadUrl") || !file.contains("_sFile") || file.value("_bIsArchived", false)) {
                continue;
            }
            const std::string candidateName = file["_sFile"].get<std::string>();
            const std::string extension = Lowercase(std::filesystem::path(candidateName).extension().string());
            if (extension == ".zip" || extension == ".o2r" || extension == ".pak") {
                filename = candidateName;
                return file["_sDownloadUrl"].get<std::string>();
            }
        }
        error = "This GameBanana mod has no supported .zip, .o2r, or .pak file.";
    } catch (const Json::exception& exception) {
        error = std::string("Could not read GameBanana file metadata: ") + exception.what();
    }
    return {};
}

static bool ExtractSupportedFiles(const std::filesystem::path& archivePath,
                                  const std::filesystem::path& destination, std::string& error) {
    int zipError = 0;
    zip_t* archive = zip_open(archivePath.string().c_str(), ZIP_RDONLY, &zipError);
    if (archive == nullptr) {
        error = "The downloaded ZIP archive could not be opened.";
        return false;
    }

    bool extracted = false;
    bool extractionFailed = false;
    uint64_t extractedBytes = 0;
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    if (count < 0 || count > 2048) {
        zip_discard(archive);
        error = "The downloaded ZIP contains too many entries.";
        return false;
    }
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i) {
        zip_stat_t stat = {};
        zip_stat_init(&stat);
        if (zip_stat_index(archive, i, 0, &stat) != 0 || stat.name == nullptr) {
            continue;
        }
        const std::filesystem::path archiveEntry = PathFromUtf8(stat.name);
        const std::filesystem::path filename = archiveEntry.filename();
        if (filename.empty() || !IsModArchive(filename)) {
            continue;
        }
        if (archiveEntry.is_absolute() || archiveEntry.has_root_name()) {
            error = "A mod archive in the ZIP has an invalid absolute path.";
            extractionFailed = true;
            break;
        }
        const auto normalizedEntry = archiveEntry.lexically_normal();
        if (normalizedEntry.empty() || *normalizedEntry.begin() == "..") {
            error = "A mod archive in the ZIP has an invalid relative path.";
            extractionFailed = true;
            break;
        }
        if (stat.size == 0 || stat.size > 256ull * 1024 * 1024) {
            error = "A mod archive inside the ZIP has an invalid or excessive size.";
            extractionFailed = true;
            break;
        }
        if (extractedBytes + stat.size > 512ull * 1024 * 1024) {
            error = "The extracted mod archives exceed the size limit.";
            extractionFailed = true;
            break;
        }
        zip_file_t* file = zip_fopen_index(archive, i, 0);
        if (file == nullptr) {
            error = "Could not read " + ToUtf8(filename.wstring()) + " from the ZIP archive.";
            extractionFailed = true;
            break;
        }
        std::string outputFilename = SafeName(ToUtf8(filename.wstring()));
        std::filesystem::path outputPath;
        const std::string optionalFolder = OptionalFolderFromArchivePath(archiveEntry);
        if (!optionalFolder.empty()) {
            outputFilename = SafeName(optionalFolder) + "__" + outputFilename;
            outputPath = destination / "_Optional" / PathFromUtf8(SafeName(optionalFolder)) /
                         PathFromUtf8(outputFilename);
        } else {
            outputPath = destination;
            for (const auto& component : normalizedEntry.parent_path()) {
                if (component == "." || component.empty()) {
                    continue;
                }
                outputPath /= PathFromUtf8(SafeName(ToUtf8(component.wstring())));
            }
            outputPath /= PathFromUtf8(outputFilename);
        }
        std::error_code filesystemError;
        if (std::filesystem::exists(outputPath, filesystemError) || filesystemError) {
            zip_fclose(file);
            error = "The ZIP contains duplicate mod archive names.";
            extractionFailed = true;
            break;
        }
        std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
        if (filesystemError) {
            zip_fclose(file);
            error = "Could not create an optional mod folder.";
            extractionFailed = true;
            break;
        }
        std::ofstream output(outputPath, std::ios::binary);
        if (!output) {
            zip_fclose(file);
            error = "Could not create " + ToUtf8(outputPath.filename().wstring()) + ".";
            extractionFailed = true;
            break;
        }
        std::array<char, 65536> buffer = {};
        zip_int64_t remaining = static_cast<zip_int64_t>(stat.size);
        bool readOk = true;
        while (remaining > 0) {
            const zip_int64_t read = zip_fread(file, buffer.data(),
                                               static_cast<zip_uint64_t>(std::min<zip_int64_t>(
                                                   remaining, static_cast<zip_int64_t>(buffer.size()))));
            if (read <= 0) {
                readOk = false;
                break;
            }
            output.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!output) {
                readOk = false;
                break;
            }
            remaining -= read;
        }
        zip_fclose(file);
        output.close();
        if (readOk && remaining == 0 && output) {
            extracted = true;
            extractedBytes += stat.size;
        } else {
            std::filesystem::remove(outputPath, filesystemError);
            error = "Could not extract " + ToUtf8(filename.wstring()) + " from the ZIP archive.";
            extractionFailed = true;
            break;
        }
    }
    if (zip_close(archive) != 0) {
        zip_discard(archive);
        error = "Could not close the downloaded ZIP archive.";
        return false;
    }
    if (extractionFailed) {
        return false;
    }
    if (!extracted) {
        error = "The archive contains no readable .o2r or .pak files.";
    }
    return extracted;
}

static DownloadResult DownloadAndInstall(const Entry& entry, const std::filesystem::path& modsDirectory,
                                         const std::shared_ptr<DownloadProgress>& progress) {
    DownloadResult result;
    std::error_code filesystemError;
    if (modsDirectory.empty() || !std::filesystem::is_directory(modsDirectory, filesystemError) || filesystemError) {
        result.error = "The mods directory is unavailable.";
        return result;
    }
    std::string url = entry.downloadUrl;
    std::string filename = entry.downloadFilename.empty() ? std::filesystem::path(entry.downloadUrl).filename().string()
                                                          : entry.downloadFilename;
    if (entry.source == Source::GameBanana) {
        url = GetGameBananaFileUrl(entry.id, filename, result.error);
        if (url.empty()) {
            return result;
        }
    }

    auto installDirectory = modsDirectory;
    if (entry.source == Source::TexturePacks) {
        installDirectory /= "Texture Packs";
        std::filesystem::create_directories(installDirectory, filesystemError);
        if (filesystemError) {
            result.error = "Could not create the texture-pack folder: " + filesystemError.message();
            return result;
        }
    }

    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto stagingDirectory = installDirectory / (".mod-download-" + std::to_string(timestamp));
    if (!std::filesystem::create_directory(stagingDirectory, filesystemError) || filesystemError) {
        result.error = "Could not create a temporary mod installation folder.";
        return result;
    }

    const auto downloadedPath = stagingDirectory / PathFromUtf8(SafeName(filename));
    const uint64_t maxDownloadBytes = entry.source == Source::TexturePacks ? 1024ull * 1024 * 1024
                                                                           : 128ull * 1024 * 1024;
    if (!HttpDownloadToFile(url, downloadedPath, maxDownloadBytes, result.error, progress.get())) {
        RemoveStagingDirectory(stagingDirectory, result.error);
        return result;
    }
    progress->transferComplete.store(true, std::memory_order_relaxed);

    if (entry.source == Source::TexturePacks) {
        if (Lowercase(downloadedPath.extension().string()) != ".zip") {
            result.error = "Emulation King returned an unexpected texture-pack file.";
            RemoveStagingDirectory(stagingDirectory, result.error);
            return result;
        }
    } else if (Lowercase(downloadedPath.extension().string()) == ".zip") {
        if (!ExtractSupportedFiles(downloadedPath, stagingDirectory, result.error)) {
            RemoveStagingDirectory(stagingDirectory, result.error);
            return result;
        }
        std::filesystem::remove(downloadedPath, filesystemError);
        if (filesystemError) {
            result.error = "Could not remove the downloaded ZIP after extraction: " + filesystemError.message();
            RemoveStagingDirectory(stagingDirectory, result.error);
            return result;
        }
    } else if (!IsModArchive(downloadedPath)) {
        result.error = "The downloaded file is not a supported .o2r, .pak, or .zip.";
        RemoveStagingDirectory(stagingDirectory, result.error);
        return result;
    }

    auto finalDirectory = modsDirectory / PathFromUtf8(SafeName(entry.name));
    if (entry.source == Source::TexturePacks) {
        finalDirectory = installDirectory / PathFromUtf8(SafeName(entry.name));
    }
    if (std::filesystem::exists(finalDirectory, filesystemError) || filesystemError) {
        filesystemError.clear();
        finalDirectory = installDirectory / PathFromUtf8(SafeName(entry.name) + " (" + SafeName(entry.id) + ")");
        if (std::filesystem::exists(finalDirectory, filesystemError) || filesystemError) {
            result.error = "A mod folder with this name already exists; it was left unchanged.";
            RemoveStagingDirectory(stagingDirectory, result.error);
            return result;
        }
    }
    std::filesystem::rename(stagingDirectory, finalDirectory, filesystemError);
    if (filesystemError) {
        result.error = "Could not move the downloaded mod into the mods folder: " + filesystemError.message();
        RemoveStagingDirectory(stagingDirectory, result.error);
        return result;
    }
    result.installed = true;
    return result;
}

static SearchResult RunSearch(Source source, const std::string& query, const std::string& modelCategory) {
    switch (source) {
        case Source::GameBanana:
            return SearchGameBanana(query);
        case Source::ModLoaderModels:
            return SearchModLoaderModels(query, modelCategory);
        case Source::ModLoaderPaks:
            return SearchModLoaderPaks(query);
        case Source::TexturePacks:
            return SearchTexturePacks(query);
    }
    return { {}, "Unknown mod catalog." };
}

static void StartSearch(TabState& state, Source source) {
    if (state.search.valid() && state.search.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const std::string query = state.query.data();
    state.lastQuery = query;
    state.runningQuery = query;
    state.lastModelCategory = state.modelCategory;
    state.runningModelCategory = state.modelCategory;
    state.error.clear();
    state.status.clear();
    state.hasSearched = true;
    const std::string modelCategory = state.modelCategory;
    state.search = std::async(std::launch::async, [source, query, modelCategory]() {
        try {
            return RunSearch(source, query, modelCategory);
        } catch (const std::exception& exception) {
            return SearchResult{ {}, std::string("Catalog search failed: ") + exception.what() };
        } catch (...) {
            return SearchResult{ {}, "Catalog search failed unexpectedly." };
        }
    });
}

static std::string ImageTextureName(const Entry& entry);
static void UnloadEntryImages(const std::vector<Entry>& entries);

static void StartThumbnailLoad(TabState& state, std::vector<Entry> entries,
                               const std::filesystem::path& modsDirectory) {
    if (entries.empty() || modsDirectory.empty()) {
        return;
    }
    if (state.thumbnails.valid() && state.thumbnails.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        state.pendingThumbnailEntries = std::move(entries);
        state.hasPendingThumbnailLoad = true;
        return;
    }
    state.thumbnails = std::async(std::launch::async, [entries = std::move(entries), modsDirectory]() mutable {
        CacheThumbnails(entries, modsDirectory);
        return entries;
    });
}

static void PollThumbnailLoad(TabState& state, const std::filesystem::path& modsDirectory) {
    if (state.thumbnails.valid() && state.thumbnails.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto loadedEntries = state.thumbnails.get();
            for (const auto& loadedEntry : loadedEntries) {
                if (loadedEntry.imagePath.empty()) {
                    continue;
                }
                const auto currentEntry =
                    std::find_if(state.entries.begin(), state.entries.end(), [&](const Entry& entry) {
                        return entry.id == loadedEntry.id && entry.imageUrl == loadedEntry.imageUrl;
                    });
                if (currentEntry != state.entries.end()) {
                    currentEntry->imagePath = loadedEntry.imagePath;
                }
            }
        } catch (const std::exception& exception) {
            SPDLOG_WARN("Could not load mod catalog thumbnails: {}", exception.what());
        } catch (...) {
            SPDLOG_WARN("Could not load mod catalog thumbnails due to an unexpected error.");
        }
    }
    if (state.hasPendingThumbnailLoad && !state.thumbnails.valid()) {
        auto entries = std::move(state.pendingThumbnailEntries);
        state.pendingThumbnailEntries.clear();
        state.hasPendingThumbnailLoad = false;
        StartThumbnailLoad(state, std::move(entries), modsDirectory);
    }
}

static void PollSearch(TabState& state, const std::filesystem::path& modsDirectory) {
    if (state.search.valid() && state.search.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            SearchResult result = state.search.get();
            if (state.runningQuery == state.query.data() && state.runningModelCategory == state.modelCategory) {
                UnloadEntryImages(state.entries);
                state.pendingThumbnailEntries.clear();
                state.hasPendingThumbnailLoad = false;
                state.entries = std::move(result.entries);
                state.error = std::move(result.error);
                StartThumbnailLoad(state, state.entries, modsDirectory);
            } else {
                state.hasSearched = false;
            }
        } catch (const std::exception& exception) {
            state.error = std::string("Catalog search failed: ") + exception.what();
        } catch (...) {
            state.error = "Catalog search failed unexpectedly.";
        }
        state.runningQuery.clear();
        state.runningModelCategory.clear();
    }
}

static bool PollDownload(TabState& state, Source source, const std::filesystem::path& modsDirectory) {
    if (!state.download.valid() || state.download.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    DownloadResult result;
    try {
        result = state.download.get();
    } catch (const std::exception& exception) {
        result.error = std::string("Mod download failed: ") + exception.what();
    } catch (...) {
        result.error = "Mod download failed unexpectedly.";
    }
    state.downloadingId.clear();
    state.downloadProgress.reset();
    state.status = result.installed ? "Download installed." : "";
    state.error = result.installed ? "" : result.error;
    return result.installed;
}

static std::string ImageTextureName(const Entry& entry) {
    return "ModCatalog-" + entry.id;
}

static void UnloadEntryImages(const std::vector<Entry>& entries) {
    auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui());
    if (gui == nullptr) {
        return;
    }
    for (const auto& entry : entries) {
        if (!entry.imagePath.empty()) {
            gui->UnloadTexture(ImageTextureName(entry));
        }
    }
}

static void DrawEntry(const Entry& entry, TabState& state, const std::filesystem::path& modsDirectory,
                      bool gameStarted, bool& installedAny) {
    ImGui::PushID((entry.id + entry.name).c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 31, 40, 220));
    ImGui::BeginChild("mod-catalog-card", ImVec2(0.0f, entry.source == Source::TexturePacks ? 128.0f : 104.0f),
                      true, ImGuiWindowFlags_NoScrollbar);
    ImGui::BeginGroup();
    const std::string textureName = ImageTextureName(entry);
    if (!entry.imagePath.empty() && !state.failedImages.contains(textureName) &&
        std::filesystem::exists(entry.imagePath)) {
        auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(
            Ship::Context::GetRawInstance()->GetWindow()->GetGui());
        if (gui != nullptr) {
            if (!gui->HasTextureByName(textureName)) {
                try {
                    gui->LoadTextureFromRawImage(textureName, entry.imagePath.string());
                } catch (const std::exception& exception) {
                    SPDLOG_WARN("Could not load catalog thumbnail {}: {}", entry.imagePath.string(), exception.what());
                    state.failedImages.insert(textureName);
                }
            }
            if (gui->HasTextureByName(textureName)) {
                const ImVec2 textureSize = gui->GetTextureSize(textureName);
                if (textureSize.x > 0.0f && textureSize.y > 0.0f) {
                    const float scale = std::min(112.0f / textureSize.x, 76.0f / textureSize.y);
                    ImGui::Image(gui->GetTextureByName(textureName),
                                 ImVec2(textureSize.x * scale, textureSize.y * scale));
                    ImGui::SameLine();
                }
            } else if (!state.failedImages.contains(textureName)) {
                state.failedImages.insert(textureName);
            }
        }
    }
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", entry.name.c_str());
    if (!entry.author.empty()) {
        ImGui::TextDisabled("%s", entry.author.c_str());
    }
    if (entry.source == Source::TexturePacks) {
        ImGui::TextDisabled("Texture packs can be downloaded but are not applied by this build.");
        for (const auto& option : entry.downloadOptions) {
            ImGui::SameLine();
            if (option.label != "Rice") {
                ImGui::TextDisabled("%s: download only", option.label.c_str());
                ImGui::SameLine();
            }
            const std::string downloadId = entry.id + "-" + option.label;
            if (state.downloadingId == downloadId) {
                ImGui::TextDisabled("Downloading...");
                continue;
            }
            if (ImGui::Button((std::string("Download ") + option.label).c_str()) && !state.download.valid()) {
                Entry selectedEntry = entry;
                selectedEntry.id = downloadId;
                selectedEntry.name += " (" + option.label + ")";
                selectedEntry.downloadUrl = option.url;
                selectedEntry.downloadFilename = option.filename;
                state.downloadingId = downloadId;
                state.error.clear();
                state.status.clear();
                state.downloadProgress = std::make_shared<DownloadProgress>();
                auto progress = state.downloadProgress;
                state.download = std::async(std::launch::async, [selectedEntry, modsDirectory, progress]() {
                    try {
                        return DownloadAndInstall(selectedEntry, modsDirectory, progress);
                    } catch (const std::exception& exception) {
                        return DownloadResult{ false, std::string("Mod download failed: ") + exception.what() };
                    } catch (...) {
                        return DownloadResult{ false, "Mod download failed unexpectedly." };
                    }
                });
            }
        }
    } else if (!entry.downloadable) {
        ImGui::TextDisabled("Open the release page to choose a download.");
    } else if (state.downloadingId == entry.id) {
        ImGui::TextDisabled("Downloading...");
    } else if (ImGui::Button("Download") && !state.download.valid()) {
        state.downloadingId = entry.id;
        state.error.clear();
        state.status.clear();
        state.downloadProgress = std::make_shared<DownloadProgress>();
        auto progress = state.downloadProgress;
        state.download = std::async(std::launch::async, [entry, modsDirectory, progress]() {
            try {
                return DownloadAndInstall(entry, modsDirectory, progress);
            } catch (const std::exception& exception) {
                return DownloadResult{ false, std::string("Mod download failed: ") + exception.what() };
            } catch (...) {
                return DownloadResult{ false, "Mod download failed unexpectedly." };
            }
        });
    }
    if (!entry.profileUrl.empty() && IsAllowedHttpsUrl(entry.profileUrl)) {
        ImGui::SameLine();
        if (ImGui::Button("Open")) {
            SDL_OpenURL(entry.profileUrl.c_str());
        }
    }
    ImGui::EndGroup();
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
    ImGui::PopID();
    installedAny = PollDownload(state, entry.source, modsDirectory) || installedAny;
    if (gameStarted && installedAny && entry.source != Source::TexturePacks) {
        ImGui::TextDisabled("New downloads require a restart before they can be enabled.");
    }
}

static void DrawCatalogTab(const char* label, TabState& state, Source source,
                           const std::filesystem::path& modsDirectory, bool gameStarted, bool& installedAny) {
    if (!ImGui::BeginTabItem(label)) {
        return;
    }
    const char* hint = source == Source::TexturePacks ? "Search texture packs..." : "Search mods...";
    const bool queryChanged = ImGui::InputTextWithHint("##catalog-search", hint, state.query.data(), state.query.size());
    if (queryChanged) {
        state.changedAt = std::chrono::steady_clock::now();
        UnloadEntryImages(state.entries);
        state.entries.clear();
        state.error.clear();
        state.status.clear();
        state.hasSearched = false;
        state.failedImages.clear();
        state.pendingThumbnailEntries.clear();
        state.hasPendingThumbnailLoad = false;
    }
    if (source == Source::ModLoaderModels) {
        static constexpr std::array<std::pair<const char*, const char*>, 5> categories = {
            std::pair{ "adult", "Adult" },       std::pair{ "child", "Child" },
            std::pair{ "combined", "Combined" }, std::pair{ "emotes", "Emotes" },
            std::pair{ "equipment", "Equipment" },
        };
        const char* selectedCategoryLabel = "Child";
        for (size_t i = 0; i < categories.size(); i++) {
            const auto& [category, categoryLabel] = categories[i];
            const bool selected = state.modelCategory == category;
            if (selected) {
                selectedCategoryLabel = categoryLabel;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.48f, 0.34f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.58f, 0.41f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.40f, 0.29f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.19f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.27f, 0.35f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.23f, 0.30f, 1.0f));
            }
            if (ImGui::Button(categoryLabel, ImVec2(104.0f, 30.0f)) && !selected) {
                state.modelCategory = category;
                state.changedAt = std::chrono::steady_clock::now();
                state.hasSearched = false;
                state.error.clear();
                state.status.clear();
                UnloadEntryImages(state.entries);
                state.entries.clear();
                state.failedImages.clear();
            }
            ImGui::PopStyleColor(3);
            if (i + 1 < categories.size()) {
                ImGui::SameLine();
            }
        }
        ImGui::TextDisabled("Showing %s models compatible with Majora's Mask.", selectedCategoryLabel);
    }
    const std::string query = state.query.data();
    if (query.empty() && source != Source::ModLoaderModels) {
        ImGui::TextDisabled("Showing featured results. Enter a search to filter the catalog.");
    }
    PollSearch(state, modsDirectory);
    if ((query != state.lastQuery || !state.hasSearched ||
         (source == Source::ModLoaderModels && state.modelCategory != state.lastModelCategory)) &&
               std::chrono::steady_clock::now() - state.changedAt >= std::chrono::milliseconds(450)) {
        StartSearch(state, source);
    }
    if (source == Source::ModLoaderPaks && !state.entries.empty()) {
        ImGui::TextDisabled("Some community PAKs target OoT and may not work with Majora's Mask.");
    } else if (source == Source::TexturePacks) {
        ImGui::TextDisabled("Texture packs are listed for download only; 2Ship does not currently apply them.");
    }
    PollThumbnailLoad(state, modsDirectory);
    installedAny = PollDownload(state, source, modsDirectory) || installedAny;
    if (!state.downloadingId.empty() && state.downloadProgress != nullptr) {
        const uint64_t received = state.downloadProgress->bytesReceived.load(std::memory_order_relaxed);
        const uint64_t total = state.downloadProgress->totalBytes.load(std::memory_order_relaxed);
        const bool requestStarted = state.downloadProgress->requestStarted.load(std::memory_order_relaxed);
        const bool transferComplete = state.downloadProgress->transferComplete.load(std::memory_order_relaxed);
        if (!requestStarted) {
            ImGui::TextDisabled("Preparing download...");
            ImGui::ProgressBar(-1.0f, ImVec2(-1.0f, 0.0f), "Preparing...");
        } else if (transferComplete) {
            ImGui::TextDisabled("Download complete. Installing...");
            ImGui::ProgressBar(1.0f, ImVec2(-1.0f, 0.0f), "Installing...");
        } else {
            const float fraction = total > 0
                                       ? static_cast<float>(std::min<double>(1.0, static_cast<double>(received) /
                                                                                         static_cast<double>(total)))
                                       : -1.0f;
            ImGui::TextDisabled(total > 0 ? "Downloading: %llu / %llu bytes"
                                          : "Downloading: %llu bytes received",
                                static_cast<unsigned long long>(received),
                                static_cast<unsigned long long>(total));
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), total > 0 ? nullptr : "Receiving...");
        }
    }
    if (!state.error.empty()) {
        ImGui::TextWrapped("%s", state.error.c_str());
    } else if (!state.status.empty()) {
        ImGui::TextColored(ImVec4(0.42f, 0.83f, 0.62f, 1.0f), "%s", state.status.c_str());
    } else if (state.search.valid()) {
        ImGui::TextDisabled("Searching...");
    }
    if (state.entries.empty() && state.hasSearched && !state.search.valid() && state.error.empty()) {
        ImGui::TextDisabled("No compatible mods found.");
    }
    ImGui::BeginChild("catalog-results", ImVec2(0.0f, 0.0f), false);
    for (const auto& entry : state.entries) {
        DrawEntry(entry, state, modsDirectory, gameStarted, installedAny);
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
}
} // namespace

bool Draw(const std::filesystem::path& modsDirectory, bool gameStarted) {
    bool installedAny = PollDownload(gameBananaState, Source::GameBanana, modsDirectory) ||
                        PollDownload(modelState, Source::ModLoaderModels, modsDirectory) ||
                        PollDownload(pakState, Source::ModLoaderPaks, modsDirectory) ||
                        PollDownload(texturePackState, Source::TexturePacks, modsDirectory);
    if (ImGui::BeginTabBar("mod-catalog-tabs")) {
        DrawCatalogTab("GameBanana", gameBananaState, Source::GameBanana, modsDirectory, gameStarted, installedAny);
        DrawCatalogTab("ModLoader64 Models", modelState, Source::ModLoaderModels, modsDirectory, gameStarted,
                       installedAny);
        DrawCatalogTab("ModLoader64 Mods", pakState, Source::ModLoaderPaks, modsDirectory, gameStarted, installedAny);
        DrawCatalogTab("Texture Packs", texturePackState, Source::TexturePacks, modsDirectory, gameStarted,
                       installedAny);
        ImGui::EndTabBar();
    }
    return installedAny;
}

static void WaitForThumbnailLoad(TabState& state) {
    if (!state.thumbnails.valid()) {
        return;
    }
    state.thumbnails.wait();
    try {
        state.thumbnails.get();
    } catch (const std::exception& exception) {
        SPDLOG_WARN("Could not finish mod catalog thumbnail loading during shutdown: {}", exception.what());
    } catch (...) {
        SPDLOG_WARN("Could not finish mod catalog thumbnail loading during shutdown.");
    }
}

void Shutdown(const std::filesystem::path& modsDirectory) {
    WaitForThumbnailLoad(gameBananaState);
    WaitForThumbnailLoad(modelState);
    WaitForThumbnailLoad(pakState);
    WaitForThumbnailLoad(texturePackState);

    if (modsDirectory.empty()) {
        return;
    }
    const auto thumbnailCacheDirectory = modsDirectory / ".mod-browser-cache";
    std::error_code filesystemError;
    std::filesystem::remove_all(thumbnailCacheDirectory, filesystemError);
    if (filesystemError) {
        SPDLOG_WARN("Could not clear mod browser thumbnail cache {}: {}", thumbnailCacheDirectory.string(),
                    filesystemError.message());
    }
}

} // namespace ModCatalog
