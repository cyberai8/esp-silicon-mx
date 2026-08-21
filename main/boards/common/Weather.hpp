#pragma once

#include "api_endpoints.h"
#include "board.h"
#include "https_request_lock.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

struct WeatherForecastDay {
    std::string date;
    std::string week;
    std::string text_day;
    std::string text_night;
    int32_t high = 0;
    int32_t low = 0;
    std::string wc_day;
    std::string wd_day;
    std::string wc_night;
    std::string wd_night;
    int32_t wind_angle_day = 0;
    int32_t wind_angle_night = 0;
    int32_t uvi = 0;
    int32_t pressure = 0;
    int32_t dpt = 0;
};

struct WeatherForecastHour {
    std::string data_time;
    std::string text;
    int32_t temp_fc = 0;
    std::string wind_class;
    std::string wind_dir;
    int32_t rh = 0;
    float prec1h = 0.f;
    int32_t clouds = 0;
    int32_t wind_angle = 0;
    int32_t pop = 0;
    int32_t uvi = 0;
    int32_t pressure = 0;
    int32_t dpt = 0;
};

struct WeatherLifeIndex {
    std::string name;
    std::string brief;
    std::string detail;
};

struct WeatherAlert {
    std::string title;
    std::string content;
};

/** 小智设备天气接口完整数据（district + dataType=all） */
struct WeatherDistrictData {
    std::string country;
    std::string province;
    std::string city;
    std::string district;
    std::string district_id;

    std::string text;
    int32_t temp = 0;
    int32_t feels_like = 0;
    int32_t rh = 0;
    std::string wind_class;
    std::string wind_dir;
    int32_t wind_angle = 0;
    float prec1h = 0.f;
    int32_t clouds = 0;
    int32_t vis = 0;
    int32_t aqi = 0;
    int32_t pm25 = 0;
    int32_t pm10 = 0;
    int32_t no2 = 0;
    int32_t so2 = 0;
    int32_t o3 = 0;
    float co = 0.f;
    int32_t uvi = 0;
    int32_t pressure = 0;
    int32_t dpt = 0;
    std::string uptime;
    std::string icon;  // 和风码，如 "101"；接口可能直接给
    std::string air;   // 公共接口：空气质量中文，如「优」

    std::vector<WeatherForecastDay> forecasts;
    std::vector<WeatherForecastHour> forecast_hours;
    std::vector<WeatherLifeIndex> indexes;
    std::vector<WeatherAlert> alerts;

    bool valid = false;
};

namespace weather_detail {

inline const char* TAG = "Weather";

inline std::string JsonStr(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    if (cJSON_IsNull(item)) {
        return "";
    }
    return "";
}

inline int32_t JsonInt(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<int32_t>(item->valueint);
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return static_cast<int32_t>(atoi(item->valuestring));
    }
    return 0;
}

inline bool JsonHas(const cJSON* obj, const char* key) {
    return obj != nullptr && cJSON_GetObjectItem(obj, key) != nullptr;
}

inline std::string JsonStrAny(const cJSON* obj, const char* a, const char* b = nullptr,
                              const char* c = nullptr) {
    std::string v = JsonStr(obj, a);
    if (v.empty() && b != nullptr) {
        v = JsonStr(obj, b);
    }
    if (v.empty() && c != nullptr) {
        v = JsonStr(obj, c);
    }
    return v;
}

inline int32_t JsonIntAny(const cJSON* obj, const char* a, const char* b = nullptr,
                          const char* c = nullptr) {
    if (JsonHas(obj, a)) {
        return JsonInt(obj, a);
    }
    if (b != nullptr && JsonHas(obj, b)) {
        return JsonInt(obj, b);
    }
    if (c != nullptr && JsonHas(obj, c)) {
        return JsonInt(obj, c);
    }
    return 0;
}

inline const char* MapWeaImgToIconCode(const std::string& wea_img) {
    // /api/public/device/weather 返回 weaImg：qing/yun/yin/yu/... → 和风资源码
    if (wea_img.empty()) {
        return nullptr;
    }
    if (wea_img == "qing") {
        return "100";
    }
    if (wea_img == "yun" || wea_img == "duoyun") {
        return "101";
    }
    if (wea_img == "yin") {
        return "104";
    }
    if (wea_img == "yu" || wea_img == "xiaoyu" || wea_img == "zhongyu" ||
        wea_img == "dayu" || wea_img == "zhenyu" || wea_img == "baoyu") {
        return "399";
    }
    if (wea_img == "lei" || wea_img == "leizhenyu") {
        return "302";
    }
    if (wea_img == "bingbao") {
        return "304";
    }
    if (wea_img == "xue" || wea_img == "xiaoxue" || wea_img == "zhongxue" ||
        wea_img == "daxue" || wea_img == "baoxue") {
        return "499";
    }
    if (wea_img == "wu" || wea_img == "dawu") {
        return "501";
    }
    if (wea_img == "mai") {
        return "502";
    }
    if (wea_img == "shachen" || wea_img == "yangsha" || wea_img == "shachenbao" ||
        wea_img == "fuchen") {
        return "503";
    }
    return nullptr;
}

inline void FillNowFields(const cJSON* obj, WeatherDistrictData& out) {
    if (obj == nullptr) {
        return;
    }
    if (out.text.empty()) {
        out.text = JsonStrAny(obj, "text", "weather", "condition");
        if (out.text.empty()) {
            out.text = JsonStrAny(obj, "textDay", "weatherText");
        }
        if (out.text.empty()) {
            out.text = JsonStr(obj, "wea");  // device/weather 简版字段
        }
    }
    if (!JsonHas(obj, "temp") && !JsonHas(obj, "temperature") && !JsonHas(obj, "tempFc")) {
        // keep existing
    } else if (!JsonHas(obj, "temp") && out.temp != 0) {
        out.temp = JsonIntAny(obj, "temperature", "tempFc");
    } else if (JsonHas(obj, "temp") || out.temp == 0) {
        out.temp = JsonIntAny(obj, "temp", "temperature", "tempFc");
    }
    if (out.feels_like == 0) {
        out.feels_like = JsonIntAny(obj, "feelsLike", "feels_like");
    }
    if (out.rh == 0) {
        out.rh = JsonIntAny(obj, "rh", "humidity");
    }
    if (out.wind_class.empty()) {
        out.wind_class = JsonStrAny(obj, "windClass", "windScale", "wind_class");
    }
    if (out.wind_dir.empty()) {
        out.wind_dir = JsonStrAny(obj, "windDir", "wind_dir", "windDirection");
    }
    if (out.icon.empty()) {
        out.icon = JsonStr(obj, "icon");
        if (out.icon.empty() && JsonHas(obj, "icon")) {
            char buf[12];
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(JsonInt(obj, "icon")));
            out.icon = buf;
        }
        if (out.icon.empty()) {
            const char* mapped = MapWeaImgToIconCode(JsonStr(obj, "weaImg"));
            if (mapped != nullptr) {
                out.icon = mapped;
            }
        }
    }
}

inline float JsonFloat(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<float>(item->valuedouble);
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return static_cast<float>(atof(item->valuestring));
    }
    return 0.f;
}

inline void ParseForecastDay(const cJSON* item, WeatherForecastDay& day) {
    day.date = JsonStr(item, "date");
    day.week = JsonStr(item, "week");
    day.text_day = JsonStr(item, "textDay");
    day.text_night = JsonStr(item, "textNight");
    day.high = JsonInt(item, "high");
    day.low = JsonInt(item, "low");
    day.wc_day = JsonStr(item, "wcDay");
    day.wd_day = JsonStr(item, "wdDay");
    day.wc_night = JsonStr(item, "wcNight");
    day.wd_night = JsonStr(item, "wdNight");
    day.wind_angle_day = JsonInt(item, "windAngleDay");
    day.wind_angle_night = JsonInt(item, "windAngleNight");
    day.uvi = JsonInt(item, "uvi");
    day.pressure = JsonInt(item, "pressure");
    day.dpt = JsonInt(item, "dpt");
}

inline void ParseForecastHour(const cJSON* item, WeatherForecastHour& hour) {
    hour.data_time = JsonStr(item, "dataTime");
    hour.text = JsonStr(item, "text");
    hour.temp_fc = JsonInt(item, "tempFc");
    hour.wind_class = JsonStr(item, "windClass");
    hour.wind_dir = JsonStr(item, "windDir");
    hour.rh = JsonInt(item, "rh");
    hour.prec1h = JsonFloat(item, "prec1h");
    hour.clouds = JsonInt(item, "clouds");
    hour.wind_angle = JsonInt(item, "windAngle");
    hour.pop = JsonInt(item, "pop");
    hour.uvi = JsonInt(item, "uvi");
    hour.pressure = JsonInt(item, "pressure");
    hour.dpt = JsonInt(item, "dpt");
}

inline void ParseLifeIndex(const cJSON* item, WeatherLifeIndex& idx) {
    idx.name = JsonStr(item, "name");
    idx.brief = JsonStr(item, "brief");
    idx.detail = JsonStr(item, "detail");
}

inline void ParseAlert(const cJSON* item, WeatherAlert& alert) {
    alert.title = JsonStr(item, "title");
    if (alert.title.empty()) {
        alert.title = JsonStr(item, "type");
    }
    alert.content = JsonStr(item, "content");
    if (alert.content.empty()) {
        alert.content = JsonStr(item, "desc");
    }
}

inline bool WeatherCodeOk(int code) {
    // district 接口成功为 0；/api/public/device/weather 成功为 200。
    return code == 0 || code == 200;
}

inline bool ParseDistrictResponse(const std::string& json, WeatherDistrictData& out) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }

    const int code = JsonInt(root, "code");
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!WeatherCodeOk(code) || !cJSON_IsObject(data)) {
        ESP_LOGW(TAG, "weather code=%d msg=%s", code, JsonStr(root, "msg").c_str());
        cJSON_Delete(root);
        return false;
    }

    out = WeatherDistrictData{};
    out.country = JsonStr(data, "country");
    out.province = JsonStr(data, "province");
    out.city = JsonStr(data, "city");
    out.district = JsonStr(data, "district");
    out.district_id = JsonStr(data, "districtId");
    out.text = JsonStrAny(data, "text", "weather", "condition");
    if (out.text.empty()) {
        out.text = JsonStr(data, "wea");  // device/weather：wea=阴/晴/...
    }
    out.temp = JsonIntAny(data, "temp", "temperature");
    out.feels_like = JsonInt(data, "feelsLike");
    out.rh = JsonIntAny(data, "rh", "humidity");
    out.wind_class = JsonStr(data, "windClass");
    out.wind_dir = JsonStr(data, "windDir");
    out.wind_angle = JsonInt(data, "windAngle");
    out.prec1h = JsonFloat(data, "prec1h");
    out.clouds = JsonInt(data, "clouds");
    out.vis = JsonInt(data, "vis");
    out.aqi = JsonInt(data, "aqi");
    out.pm25 = JsonInt(data, "pm25");
    out.pm10 = JsonInt(data, "pm10");
    out.no2 = JsonInt(data, "no2");
    out.so2 = JsonInt(data, "so2");
    out.o3 = JsonInt(data, "o3");
    out.co = JsonFloat(data, "co");
    out.uvi = JsonInt(data, "uvi");
    out.pressure = JsonInt(data, "pressure");
    out.dpt = JsonInt(data, "dpt");
    out.uptime = JsonStrAny(data, "uptime", "updatedAt");
    out.icon = JsonStr(data, "icon");
    if (out.icon.empty() && JsonHas(data, "icon")) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(JsonInt(data, "icon")));
        out.icon = buf;
    }
    if (out.icon.empty()) {
        const char* mapped = MapWeaImgToIconCode(JsonStr(data, "weaImg"));
        if (mapped != nullptr) {
            out.icon = mapped;
        }
    }
    out.air = JsonStr(data, "air");
    if (out.city.empty()) {
        out.city = JsonStrAny(data, "cityName", "name", "location");
    }
    FillNowFields(data, out);
    const cJSON* now = cJSON_GetObjectItem(data, "now");
    if (cJSON_IsObject(now)) {
        FillNowFields(now, out);
        if (out.city.empty()) {
            out.city = JsonStrAny(now, "city", "cityName", "name");
        }
    }

    const std::string alarm = JsonStr(data, "alarm");
    if (!alarm.empty()) {
        WeatherAlert alert;
        alert.title = alarm;
        out.alerts.push_back(std::move(alert));
    }

    const cJSON* forecasts = cJSON_GetObjectItem(data, "forecasts");
    if (cJSON_IsArray(forecasts)) {
        const int n = cJSON_GetArraySize(forecasts);
        out.forecasts.reserve(n);
        for (int i = 0; i < n; ++i) {
            const cJSON* item = cJSON_GetArrayItem(forecasts, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            WeatherForecastDay day;
            ParseForecastDay(item, day);
            out.forecasts.push_back(std::move(day));
        }
    }

    if (out.text.empty() && !out.forecasts.empty()) {
        out.text = out.forecasts.front().text_day;
    }

    const cJSON* hours = cJSON_GetObjectItem(data, "forecastHours");
    if (cJSON_IsArray(hours)) {
        const int n = cJSON_GetArraySize(hours);
        out.forecast_hours.reserve(n);
        for (int i = 0; i < n; ++i) {
            const cJSON* item = cJSON_GetArrayItem(hours, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            WeatherForecastHour hour;
            ParseForecastHour(item, hour);
            out.forecast_hours.push_back(std::move(hour));
        }
    }

    if (out.text.empty() && !out.forecast_hours.empty()) {
        out.text = out.forecast_hours.front().text;
    }

    const cJSON* indexes = cJSON_GetObjectItem(data, "indexes");
    if (cJSON_IsArray(indexes)) {
        const int n = cJSON_GetArraySize(indexes);
        out.indexes.reserve(n);
        for (int i = 0; i < n; ++i) {
            const cJSON* item = cJSON_GetArrayItem(indexes, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            WeatherLifeIndex idx;
            ParseLifeIndex(item, idx);
            out.indexes.push_back(std::move(idx));
        }
    }

    const cJSON* alerts = cJSON_GetObjectItem(data, "alerts");
    if (cJSON_IsArray(alerts)) {
        const int n = cJSON_GetArraySize(alerts);
        out.alerts.reserve(n);
        for (int i = 0; i < n; ++i) {
            const cJSON* item = cJSON_GetArrayItem(alerts, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            WeatherAlert alert;
            ParseAlert(item, alert);
            if (!alert.title.empty() || !alert.content.empty()) {
                out.alerts.push_back(std::move(alert));
            }
        }
    }

    if (out.forecasts.empty()) {
        const int32_t high = JsonIntAny(data, "tempHigh", "high");
        const int32_t low = JsonIntAny(data, "tempLow", "low");
        if (high != 0 || low != 0) {
            WeatherForecastDay day;
            day.high = high;
            day.low = low;
            day.text_day = out.text;
            out.forecasts.push_back(std::move(day));
        }
    }

    cJSON_Delete(root);
    out.valid = !out.text.empty() || !out.icon.empty() || !out.forecasts.empty() ||
                out.temp != 0;
    return out.valid;
}

}  // namespace weather_detail

/** 小智设备天气服务：按 districtId 拉取，不缓存完整 JSON 到 NVS */
class WeatherService {
public:
    static constexpr const char* kDefaultDistrictId = "440306";

    static WeatherService& Instance() {
        static WeatherService instance;
        return instance;
    }

    void SetDistrictId(const std::string& id) {
        if (!id.empty()) {
            district_id_ = id;
        }
    }

    const std::string& DistrictId() const { return district_id_; }

    esp_err_t Fetch(WeatherDistrictData& out) {
        esp_err_t err = FetchFromNetwork(out);
        if (err == ESP_OK) {
            cached_ = out;
        }
        return err;
    }

    const WeatherDistrictData& Cached() const { return cached_; }

    // 待机用：GET /api/public/device/weather/{mac}
    esp_err_t FetchByDevice(WeatherDistrictData& out) {
        esp_err_t err = FetchDeviceFromNetwork(out);
        if (err == ESP_OK) {
            device_cached_ = out;
        }
        return err;
    }

    const WeatherDistrictData& DeviceCached() const { return device_cached_; }

private:
    WeatherService() : district_id_(kDefaultDistrictId) {}

    esp_err_t HttpGetWeather(const std::string& url, WeatherDistrictData& out) {
        std::lock_guard<std::mutex> https_guard(HttpsRequestLock());
        if (!HttpsInternalRamReady()) {
            ESP_LOGW(weather_detail::TAG,
                     "HTTPS deferred, largest_int=%u spiram=%u internal=%u",
                     static_cast<unsigned>(HttpsLargestInternalBlock()),
                     static_cast<unsigned>(HttpsLargestSpiramBlock()),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            return ESP_ERR_NO_MEM;
        }
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            ESP_LOGE(weather_detail::TAG, "Network not available");
            return ESP_ERR_INVALID_STATE;
        }

        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(weather_detail::TAG, "GET weather");
        http->SetTimeout(10000);
        http->SetHeader("Accept", "application/json");
        http->SetHeader("Connection", "close");
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());

        if (!http->Open("GET", url.c_str())) {
            ESP_LOGE(weather_detail::TAG, "HTTP open failed");
            return ESP_FAIL;
        }

        const int status = http->GetStatusCode();
        if (status != 200) {
            ESP_LOGE(weather_detail::TAG, "HTTP status=%d", status);
            http->Close();
            return ESP_FAIL;
        }

        std::string body = http->ReadAll();
        http->Close();

        if (body.empty()) {
            ESP_LOGE(weather_detail::TAG, "Empty body");
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (!weather_detail::ParseDistrictResponse(body, out)) {
            ESP_LOGE(weather_detail::TAG, "Parse failed, len=%u body=%.160s",
                     static_cast<unsigned>(body.size()), body.c_str());
            return ESP_ERR_INVALID_RESPONSE;
        }

        ESP_LOGI(weather_detail::TAG, "OK %s %s %d°C text=%s icon=%s days=%u hours=%u",
                 out.province.c_str(), out.district.c_str(), out.temp,
                 out.text.c_str(), out.icon.c_str(),
                 static_cast<unsigned>(out.forecasts.size()),
                 static_cast<unsigned>(out.forecast_hours.size()));
        return ESP_OK;
    }

    esp_err_t FetchFromNetwork(WeatherDistrictData& out) {
        return HttpGetWeather(api::WeatherDistrictUrl(district_id_), out);
    }

    esp_err_t FetchDeviceFromNetwork(WeatherDistrictData& out) {
        const std::string mac = SystemInfo::GetMacAddress();
        ESP_LOGI(weather_detail::TAG, "device weather mac=%s", mac.c_str());
        return HttpGetWeather(api::DeviceWeatherUrl(mac), out);
    }

    std::string district_id_;
    WeatherDistrictData cached_;
    WeatherDistrictData device_cached_;
};
