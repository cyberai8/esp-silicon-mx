#pragma once

// 仿真替身：不走网络，提供一份可看的待机天气样例数据。
#include <esp_err.h>

#include <cstdint>
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
    std::string icon;
    std::string air;
    std::vector<WeatherForecastDay> forecasts;
    std::vector<WeatherForecastHour> forecast_hours;
    std::vector<WeatherLifeIndex> indexes;
    std::vector<WeatherAlert> alerts;
    bool valid = false;
};

class WeatherService {
public:
    static WeatherService& Instance() {
        static WeatherService inst;
        return inst;
    }

    esp_err_t FetchByDevice(WeatherDistrictData& out) {
        out = DeviceCached();
        return out.valid ? ESP_OK : ESP_FAIL;
    }

    const WeatherDistrictData& DeviceCached() const { return cached_; }

private:
    WeatherService() {
        cached_.valid = true;
        cached_.city = "安阳";
        cached_.district = "滑县";
        cached_.text = "晴";
        cached_.temp = 30;
        cached_.rh = 86;
        cached_.air = "优";
        cached_.icon = "100";
        cached_.wind_dir = "东北风";
        cached_.wind_class = "1级";
        WeatherForecastDay day;
        day.high = 32;
        day.low = 24;
        day.text_day = "晴";
        cached_.forecasts.push_back(day);
    }

    WeatherDistrictData cached_;
};
