#include "api_endpoints.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "settings.h"

namespace api {

namespace {

constexpr const char* TAG = "ApiEndpoints";
constexpr const char* kDefaultApiBase = "https://api.tenclass.net";

}  // namespace

std::string GetApiBaseUrl() {
    Settings settings("wifi", false);
    std::string api_base = settings.GetString("api_base_url");
    if (!api_base.empty()) {
        while (!api_base.empty() && api_base.back() == '/') {
            api_base.pop_back();
        }
        ESP_LOGI(TAG, "Using api_base_url from NVS: %s", api_base.c_str());
        return api_base;
    }

    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        ESP_LOGW(TAG, "Invalid ota_url %s, fallback to %s", url.c_str(),
                 kDefaultApiBase);
        return kDefaultApiBase;
    }
    const size_t path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return url;
    }
    return url.substr(0, path_start);
}

std::string Url(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return GetApiBaseUrl();
    }
    return GetApiBaseUrl() + path;
}

std::string RedactClawUrlsForLog(const std::string& text) {
    std::string out = text;
    const std::string base = GetApiBaseUrl();
    size_t pos = 0;
    while ((pos = out.find(base, pos)) != std::string::npos) {
        size_t end = out.find_first_of("\"' \t\r\n,}", pos + base.size());
        if (end == std::string::npos) {
            end = out.size();
        }
        out.replace(pos, end - pos, "[redacted]");
        pos += 10;
    }
    return out;
}

}  // namespace api
