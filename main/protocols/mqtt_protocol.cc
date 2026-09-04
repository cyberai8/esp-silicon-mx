#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"
#include <wifi_station.h>
#if CONFIG_BOARD_TYPE_ESP_SHOW || CONFIG_BOARD_TYPE_ESP_VOCAT
#include <esp_wifi.h>
#endif

#include <esp_log.h>
#include <cstring>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

namespace {
constexpr unsigned kWifiRecoveryFailureThreshold = 3;
}

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            static_cast<MqttProtocol*>(arg)->ScheduleReconnect();
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    shutting_down_.store(true);
    reconnect_pending_.store(false);
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

void MqttProtocol::ArmReconnect(uint32_t delay_ms) {
    if (shutting_down_.load() || reconnect_timer_ == nullptr) {
        return;
    }

    reconnect_pending_.store(true);
    if (esp_timer_is_active(reconnect_timer_)) {
        esp_timer_stop(reconnect_timer_);
    }
    const esp_err_t err = esp_timer_start_once(
        reconnect_timer_, static_cast<uint64_t>(delay_ms) * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to arm MQTT reconnect timer: %s", esp_err_to_name(err));
    }
}

void MqttProtocol::ScheduleReconnect() {
    if (shutting_down_.load()) {
        return;
    }
    if (!WifiStation::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "WiFi is not ready, postpone MQTT reconnect");
        ArmReconnect();
        return;
    }
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    if (state == kDeviceStateUpgrading || state == kDeviceStateWifiConfiguring) {
        ArmReconnect();
        return;
    }
    if (connect_in_progress_.load()) {
        ESP_LOGI(TAG, "MQTT connect is already in progress, postpone reconnect");
        ArmReconnect();
        return;
    }
    ESP_LOGI(TAG, "Reconnecting to MQTT server");
    app.Schedule([this]() {
        if (shutting_down_.load()) {
            return;
        }
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        if (!WifiStation::GetInstance().IsConnected() ||
            state == kDeviceStateUpgrading ||
            state == kDeviceStateWifiConfiguring ||
            connect_in_progress_.load()) {
            ArmReconnect();
            return;
        }
        if (mqtt_ != nullptr && mqtt_->IsConnected()) {
            reconnect_pending_.store(false);
            return;
        }
        reconnect_pending_.store(false);
        StartMqttClient(false);
    });
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (shutting_down_.load()) {
        return false;
    }
    if (connect_in_progress_.exchange(true)) {
        ESP_LOGI(TAG, "MQTT connect is already in progress");
        return false;
    }
    struct ConnectGuard {
        std::atomic<bool>& flag;
        ~ConnectGuard() { flag.store(false); }
    } connect_guard{connect_in_progress_};

    reconnect_pending_.store(false);
    if (reconnect_timer_ != nullptr && esp_timer_is_active(reconnect_timer_)) {
        esp_timer_stop(reconnect_timer_);
    }

    if (mqtt_ != nullptr) {
        ESP_LOGI(TAG, "Destroy stale MQTT client before reconnect");
        mqtt_generation_.fetch_add(1);
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", MQTT_PING_INTERVAL_SECONDS);
    if (keepalive_interval <= 0 || keepalive_interval > MQTT_PING_INTERVAL_SECONDS) {
        keepalive_interval = MQTT_PING_INTERVAL_SECONDS;
    }
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    const uint32_t generation = mqtt_generation_.fetch_add(1) + 1;
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this, generation]() {
        if (shutting_down_.load() || mqtt_generation_.load() != generation) {
            return;
        }
        xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_DISCONNECTED_EVENT);
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        ArmReconnect();

        // MQTT 回调任务里不能析构自身。转到应用主任务关闭失效的 UDP 会话，
        // 并销毁仍未自动恢复的客户端，避免后台自动重连与定时重连互相打架。
        Application::GetInstance().Schedule([this, generation]() {
            if (shutting_down_.load() || mqtt_generation_.load() != generation) {
                return;
            }

            bool had_audio_channel = false;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                had_audio_channel = udp_ != nullptr;
                udp_.reset();
            }
            session_id_.clear();

            if (mqtt_ != nullptr && !mqtt_->IsConnected()) {
                mqtt_generation_.fetch_add(1);
                mqtt_.reset();
            }
            if (had_audio_channel && on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_();
            }
        });
    });

    mqtt_->OnConnected([this, generation]() {
        if (shutting_down_.load() || mqtt_generation_.load() != generation) {
            return;
        }
        consecutive_connect_failures_.store(0);
        reconnect_pending_.store(false);
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        if (reconnect_timer_ != nullptr && esp_timer_is_active(reconnect_timer_)) {
            esp_timer_stop(reconnect_timer_);
        }
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    const int64_t connect_started_us = esp_timer_get_time();
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        const int64_t connect_elapsed_ms =
            (esp_timer_get_time() - connect_started_us) / 1000;
        const int error = mqtt_->GetLastError();
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d elapsed=%lld ms",
                 error, static_cast<long long>(connect_elapsed_ms));

        // EspMqtt 在 Connect() 超时后仍保留内部自动重连任务。立即销毁它，
        // 后续只允许本类的定时器发起一次连接，避免多个连接流程叠加。
        mqtt_generation_.fetch_add(1);
        mqtt_.reset();

        // 只有完整耗尽 10 秒连接窗口才视为链路超时。认证拒绝等快速失败
        // 不重置 WiFi，避免服务器配置问题影响音乐/天气等其它联网功能。
        if (connect_elapsed_ms >= 9000) {
            const unsigned failures =
                consecutive_connect_failures_.fetch_add(1) + 1;
            if (failures >= kWifiRecoveryFailureThreshold) {
                if (RequestWifiLinkRecovery()) {
                    consecutive_connect_failures_.store(0);
                } else {
                    // 非语音页面先不动 WiFi；保留到阈值前一档，用户进入
                    // 数字人后若仍超时，下一次即可执行链路恢复。
                    consecutive_connect_failures_.store(
                        kWifiRecoveryFailureThreshold - 1);
                }
            }
        } else {
            consecutive_connect_failures_.store(0);
        }
        ArmReconnect();
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
        return false;
    }

    consecutive_connect_failures_.store(0);
    reconnect_pending_.store(false);
    error_occurred_ = false;
    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::RequestWifiLinkRecovery() {
#if CONFIG_BOARD_TYPE_ESP_SHOW || CONFIG_BOARD_TYPE_ESP_VOCAT
    const auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateConnecting &&
        state != kDeviceStateListening &&
        state != kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Defer WiFi reassociation while voice chat is not active");
        return false;
    }
    if (!WifiStation::GetInstance().IsConnected()) {
        return true;
    }

    ESP_LOGW(TAG,
             "MQTT failed %u consecutive times while WiFi still has an IP; "
             "reassociate WiFi without rebooting",
             kWifiRecoveryFailureThreshold);
    const esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi soft reconnect request failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
#else
    ESP_LOGW(TAG, "MQTT failed %u consecutive times", kWifiRecoveryFailureThreshold);
    return false;
#endif
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty() || mqtt_ == nullptr) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet->payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet->timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet->payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    if (mqtt_ != nullptr) {
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        if (connect_in_progress_.load() || reconnect_pending_.load()) {
            ESP_LOGI(TAG, "MQTT reconnect is pending; wait for the single reconnect flow");
            return false;
        }
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_,
                         MQTT_PROTOCOL_SERVER_HELLO_EVENT |
                         MQTT_PROTOCOL_DISCONNECTED_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        MQTT_PROTOCOL_SERVER_HELLO_EVENT | MQTT_PROTOCOL_DISCONNECTED_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (bits & MQTT_PROTOCOL_DISCONNECTED_EVENT) {
        ESP_LOGE(TAG, "MQTT disconnected while waiting for server hello");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "Invalid audio packet size: %u", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        if (sequence < remote_sequence_) {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, remote_sequence_ + 1);
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)packet->payload.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
