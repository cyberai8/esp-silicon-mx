#pragma once

#include <cstdio>
#include <cstring>
#include <string>

// 设为 1 时在 HTTP 调用处打印请求 URL、请求体与响应体。
#ifndef API_HTTP_DEBUG
#define API_HTTP_DEBUG 0
#endif

#if API_HTTP_DEBUG
#include "esp_log.h"
#endif

namespace api {

#if API_HTTP_DEBUG

namespace detail {

inline const char* HttpBodyForLog(const std::string& body) {
    return body.empty() ? "(empty)" : body.c_str();
}

}  // namespace detail

inline void LogHttpRequest(const char* tag, const char* method,
                           const std::string& url,
                           const std::string& body = std::string(),
                           const char* extra = nullptr) {
    ESP_LOGI(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGI(tag, "[API_HTTP] >>> %s", extra);
    }
    ESP_LOGI(tag, "[API_HTTP] >>> body=%s", detail::HttpBodyForLog(body));
}

inline void LogHttpBinaryRequest(const char* tag, const char* method,
                                 const std::string& url, size_t body_bytes,
                                 const char* extra = nullptr) {
    ESP_LOGI(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGI(tag, "[API_HTTP] >>> %s", extra);
    }
    ESP_LOGI(tag, "[API_HTTP] >>> body=(binary %u bytes)",
              static_cast<unsigned>(body_bytes));
}

inline void LogHttpResponse(const char* tag, int status,
                            const std::string& body) {
    ESP_LOGI(tag, "[API_HTTP] <<< status=%d body=%s", status,
             detail::HttpBodyForLog(body));
}

#else

inline void LogHttpRequest(const char*, const char*, const std::string&,
                           const std::string& = std::string(),
                           const char* = nullptr) {}

inline void LogHttpBinaryRequest(const char*, const char*, const std::string&,
                                 size_t, const char* = nullptr) {}

inline void LogHttpResponse(const char*, int, const std::string&) {}

#endif

// 从 NVS api_base_url 读取；否则从 ota_url（或 CONFIG_OTA_URL）解析 scheme://host。
// NVS 示例：wifi.api_base_url = https://your-server.com
std::string GetApiBaseUrl();

constexpr const char* kApiV1Prefix = "/api/v1";
constexpr const char* kXiaozhiDevicePrefix = "/xiaozhi/device";

// ASR
constexpr const char* kAsrTranscribe = "/api/v1/asr/transcribe";
constexpr const char* kAsrAudioRecords =
    "/api/v1/asr/audio-records?originalName=";
// 设备侧原始 WAV 转写（octet-stream body）
constexpr const char* kXiaozhiAsrWav = "/xiaozhi/api/asr?format=wav";

// DashScope 文生图
constexpr const char* kText2Image = "/xiaozhi/api/dashscope/text2image";
constexpr const char* kText2ImageTaskFmt =
    "/xiaozhi/api/dashscope/text2image/tasks/%s?maxSide=450";

// Sonicloud 实时同声传译：换 Token，返回 data.wsUrl
constexpr const char* kSinicloudToken = "/xiaozhi/api/sinicloud/token";

// Weather
constexpr const char* kWeatherDistrictPath =
    "/api/v1/weather/district?dataType=all&districtId=";
// 待机天气：GET {base}/api/public/device/weather/{mac}
constexpr const char* kDeviceWeatherPath = "/api/public/device/weather/";

// GPS
constexpr const char* kGpsLocationReport =
    "/xiaozhi/device/gps/location/report/cell";
constexpr const char* kGpsStaticMap =
    "/xiaozhi/device/gps/location/static-map";

std::string Url(const char* path);

inline std::string WeatherDistrictUrl(const std::string& district_id) {
    return Url(kWeatherDistrictPath) + district_id;
}

inline std::string DeviceWeatherUrl(const std::string& mac) {
    return Url(kDeviceWeatherPath) + mac;
}

inline std::string AsrAudioRecordsUrl(const char* original_name) {
    if (original_name == nullptr || original_name[0] == '\0') {
        return Url(kAsrAudioRecords);
    }
    return Url(kAsrAudioRecords) + original_name;
}

inline std::string Text2ImageTaskUrl(const std::string& task_id) {
    char path[192];
    std::snprintf(path, sizeof(path), kText2ImageTaskFmt, task_id.c_str());
    return Url(path);
}

// 日志脱敏：响应体等可能含 staticMapUrl 等完整地址，禁止输出 API 域名 URL。
std::string RedactClawUrlsForLog(const std::string& text);

}  // namespace api
