#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

#include "core/el_debug.h"
#include "core/el_types.h"
#include "porting/el_device.h"
#include "porting/el_misc.h"
#include "porting/el_transport.h"
#include "sscma/definations.hpp"
#include "sscma/interface/wifi.hpp"
#include "sscma/prototypes.hpp"
#include "sscma/types.hpp"
#include "sscma/utility.hpp"

namespace sscma {

using namespace edgelab;

using namespace sscma::types;
using namespace sscma::prototypes;
using namespace sscma::utility;

namespace types {

typedef enum class mqtt_sta_e { UNKNOWN = 0, DISCONNECTED, CONNECTED, ACTIVE } mqtt_sta_e;

}

namespace transport {

class MQTT final : public Supervisable, public Transport {
   public:
    MQTT(StatefulInterface* interface)
        : _interface(interface),
          _network(Device::get_device()->get_network()),
          _device_lock(),
          _size(SSCMA_CMD_MAX_LENGTH << 1),  // 2 times of the max length of command
          _buffer(new char[_size]{}),
          _head(0),
          _tail(0),
          _mqtt_server_config(std::pair<mqtt_sta_e, mqtt_server_config_t>{
            mqtt_sta_e::UNKNOWN, get_default_mqtt_server_config(Device::get_device())}),
          _mqtt_pubsub_config(),
          _sub_topics_set() {
        EL_ASSERT(_interface);
        EL_ASSERT(_network);
        EL_ASSERT(_buffer);
        _mqtt_handler_ptr   = this;
        _mqtt_pubsub_config = get_default_mqtt_pubsub_config(Device::get_device());

        _interface->add_pre_down_callback(this, [](void* from) {
            auto mqtt = static_cast<MQTT*>(from);
            mqtt->set_down({mqtt_sta_e::DISCONNECTED, mqtt->get_last_mqtt_server_config()});
        });

        this->_is_present = true;
    }

    ~MQTT() {
        this->_is_present = false;

        _interface->remove_pre_down_callback(this);

        _mqtt_handler_ptr = nullptr;
        _interface        = nullptr;
        _network          = nullptr;

        if (_buffer) [[likely]] {
            delete[] _buffer;
            _buffer = nullptr;
        }
    }

    void poll_from_supervisor() override {
        EL_LOGI("[SSCMA] MQTT::poll_from_supervisor()");

        if (_mqtt_server_config.is_synchorized()) [[likely]] {
            const auto& config      = _mqtt_server_config.load_last();
            auto        current_sta = sync_status_from_driver();
            if (current_sta < config.second.first) [[unlikely]]
                bring_up(config.second);
        } else {
            auto config = _mqtt_server_config.load();
            if (set_down(config.second) && bring_up(config.second)) _mqtt_server_config.synchorize(std::move(config));
        }
    }

    bool set_mqtt_server_config(mqtt_server_config_t mqtt_server_config) {
        // use default port if not specified
        if (mqtt_server_config.port == 0) [[unlikely]] {
            if (mqtt_server_config.use_ssl)
                mqtt_server_config.port = SSCMA_MQTT_DEFAULT_SSL_PORT;
            else
                mqtt_server_config.port = SSCMA_MQTT_DEFAULT_PORT;
        }
        _mqtt_server_config.store(std::pair<mqtt_sta_e, mqtt_server_config_t>{
          std::strlen(mqtt_server_config.address) ? mqtt_sta_e::ACTIVE : mqtt_sta_e::DISCONNECTED, mqtt_server_config});
        return true;
    }

    mqtt_server_config_t get_mqtt_server_config() const { return _mqtt_server_config.load().second.second; }

    mqtt_server_config_t get_last_mqtt_server_config() const { return _mqtt_server_config.load_last().second.second; }

    bool is_mqtt_server_config_synchornized() const { return _mqtt_server_config.is_synchorized(); }

    bool is_mqtt_server_connected() const { return sync_status_from_driver() >= mqtt_sta_e::CONNECTED; }

    mqtt_pubsub_config_t get_mqtt_pubsub_config() const { return _mqtt_pubsub_config; }

    el_err_code_t read_bytes(char* buffer, std::size_t size) override {
        if (!buffer | !size) [[unlikely]]
            return EL_EINVAL;

        auto        head = _head;
        auto        tail = _tail;
        std::size_t len  = 0;

        while (tail != head && len < size) {
            buffer[len++] = _buffer[tail];
            tail          = (tail + 1) % _size;
        }
        _tail = tail;

        return EL_OK;
    }

    el_err_code_t send_bytes(const char* buffer, std::size_t size) override {
        return m_send_bytes(_mqtt_pubsub_config.pub_topic, _mqtt_pubsub_config.pub_qos, buffer, size);
    }

    char echo(bool only_visible = true) override {
        auto c = get_char();
        if (only_visible && !std::isprint(c)) return c;
        m_send_bytes(_mqtt_pubsub_config.pub_topic, _mqtt_pubsub_config.pub_qos, &c, 1);
        return c;
    }

    char get_char() override {
        auto head = _head;
        auto tail = _tail;
        if (head == tail) [[unlikely]]
            return '\0';
        return _buffer[_tail = (tail + 1) % _size];
    }

    std::size_t get_line(char* buffer, std::size_t size, const char delim = 0x0d) override {
        if (!buffer | !size) [[unlikely]]
            return 0;

        auto              head    = _head;
        auto              tail    = _tail;
        auto              prev    = tail;
        std::size_t const len_max = size - 1;
        std::size_t       len     = 0;

        for (char c = _buffer[tail]; (tail != head) & (len < len_max); tail = (tail + 1) % _size, ++len) {
            if ((c == delim) | (c == '\0')) {
                tail = (tail + 1) % _size;
                len += c != '\0';
                break;
            }
        }

        if (len) {
            for (std::size_t i = 0; i < len; ++i) buffer[i] = _buffer[(prev + i) % _size];
            _tail = tail;
        }

        return len;
    }

    void emit_mqtt_discover() {
        const auto& ss{concat_strings("\r", mqtt_pubsub_config_2_json_str(_mqtt_pubsub_config), "\n")};
        char        discover_topic[SSCMA_MQTT_TOPIC_LEN]{};
        std::snprintf(
          discover_topic, sizeof(discover_topic) - 1, SSCMA_MQTT_DISCOVER_TOPIC, SSCMA_AT_API_MAJOR_VERSION);

        m_send_bytes(discover_topic, 0, ss.c_str(), ss.size());
    }

   protected:
    static inline void mqtt_subscribe_callback(char* top, int tlen, char* msg, int mlen) {
        if (!_mqtt_handler_ptr) [[unlikely]]
            return;
        auto this_ptr = static_cast<MQTT*>(_mqtt_handler_ptr);

        if (tlen ^ std::strlen(this_ptr->_mqtt_pubsub_config.sub_topic) ||
            std::strncmp(top, this_ptr->_mqtt_pubsub_config.sub_topic, tlen) || mlen <= 1)
            return;

        if (!this_ptr->push_to_buffer(msg, mlen)) [[unlikely]]
            EL_LOGI("[SSCMA] MQTT::mqtt_subscribe_callback() - buffer may corrupted");
    }

    inline bool push_to_buffer(const char* bytes, std::size_t size) {
        if (!bytes | !size) [[unlikely]]
            return true;

        auto head      = _head;
        auto tail      = _tail;
        bool corrupted = false;

        for (std::size_t i = 0; i < size; ++i) {
            _buffer[head] = bytes[i];
            head          = (head + 1) % _size;
            corrupted |= head == tail;
        }
        if (corrupted) [[unlikely]]
            _tail = head;
        _head = head;

        return !corrupted;
    }

    inline el_err_code_t m_send_bytes(const char* topic, uint8_t qos, const char* buffer, std::size_t size) {
        const Guard guard(_device_lock);
        if (this->_is_present == false) [[unlikely]]
            return EL_EPERM;
        return _network->publish(topic, buffer, size, static_cast<mqtt_qos_t>(qos));
    }

    bool bring_up(const std::pair<mqtt_sta_e, mqtt_server_config_t>& config) {
        EL_LOGI("[SSCMA] MQTT::bring_up() checking interface status");
        if (!_interface->is_interface_up()) [[unlikely]]
            return false;

        auto current_sta = sync_status_from_driver();

        EL_LOGI("[SSCMA] MQTT::bring_up() current status: %d, target status: %d", current_sta, config.first);

        if (current_sta == mqtt_sta_e::DISCONNECTED) {
            if (current_sta >= config.first) return true;
            EL_LOGI("[SSCMA] MQTT::bring_up() driver connect: %s:%d", config.second.address, config.second.port);
            // TODO: driver change topic callback API
            auto ret = _network->connect(config.second, mqtt_subscribe_callback);  // driver connect
            if (ret != EL_OK) [[unlikely]]
                return false;
            this->_is_present = true;
            current_sta =
              try_ensure_wifi_status_changed_from(current_sta, SSCMA_MQTT_POLL_DELAY_MS, SSCMA_MQTT_POLL_RETRY);
        }

        if (current_sta == mqtt_sta_e::CONNECTED) {
            if (current_sta >= config.first) return true;
            EL_LOGI("[SSCMA] MQTT::bring_up() driver subscribe: %s", _mqtt_pubsub_config.sub_topic);
            auto ret = _network->subscribe(_mqtt_pubsub_config.sub_topic,
                                           static_cast<mqtt_qos_t>(_mqtt_pubsub_config.sub_qos));  // driver subscribe
            if (ret != EL_OK) [[unlikely]]
                return false;
            _sub_topics_set.emplace(_mqtt_pubsub_config.sub_topic);
            current_sta = sync_status_from_driver();  // sync internal status
        }

        if (current_sta == mqtt_sta_e::ACTIVE) {
            emit_mqtt_discover();
            return true;
        }

        return false;
    }

    bool set_down(const std::pair<mqtt_sta_e, mqtt_server_config_t>& config) {
        const Guard guard(_device_lock);

        auto current_sta = sync_status_from_driver();

        EL_LOGI("[SSCMA] MQTT::set_down() current status: %d, target status: %d", current_sta, config.first);

        if (current_sta == mqtt_sta_e::ACTIVE) {
            if (current_sta < config.first) return true;
            EL_LOGI("[SSCMA] MQTT::set_down() driver unsubscribe topic count: %d", _sub_topics_set.size());
            for (const auto& sub_topic : _sub_topics_set) _network->unsubscribe(sub_topic.c_str());
            _sub_topics_set.clear();                  // clear old topics
            current_sta = sync_status_from_driver();  // sync internal status
        }

        if (current_sta == mqtt_sta_e::CONNECTED) {
            if (current_sta < config.first) return true;
            EL_LOGI("[SSCMA] MQTT::set_down() - driver disconnect()");
            auto ret = _network->disconnect();  // driver disconnect
            if (ret != EL_OK) [[unlikely]]
                return false;
            this->_is_present = false;
            current_sta       = ensure_status_changed_from(current_sta, SSCMA_MQTT_POLL_DELAY_MS);
        }

        if (current_sta == mqtt_sta_e::DISCONNECTED) return true;

        return false;
    }

    mqtt_sta_e try_ensure_wifi_status_changed_from(mqtt_sta_e old_sta, uint32_t delay_ms, std::size_t n_times) {
        auto sta = sync_status_from_driver();
        for (std::size_t i = 0; (i < n_times) & (sta != old_sta); ++i) {
            el_sleep(delay_ms);
            sta = sync_status_from_driver();
        }
        return sta;
    }

    mqtt_sta_e ensure_status_changed_from(mqtt_sta_e old_sta, uint32_t delay_ms) {
        auto sta = sync_status_from_driver();
        for (; sta == old_sta;) {
            el_sleep(delay_ms);
            sta = sync_status_from_driver();
        }
        return sta;
    }

    mqtt_sta_e sync_status_from_driver() const {
        auto driver_sta = _network->status();
        switch (driver_sta) {
        case NETWORK_LOST:
            return mqtt_sta_e::DISCONNECTED;
        case NETWORK_IDLE:
            return mqtt_sta_e::DISCONNECTED;
        case NETWORK_JOINED:
            return mqtt_sta_e::DISCONNECTED;
        case NETWORK_CONNECTED:
            if (_sub_topics_set.size()) return mqtt_sta_e::ACTIVE;
            return mqtt_sta_e::CONNECTED;
        default:
            return mqtt_sta_e::UNKNOWN;
        }
    }

   private:
    StatefulInterface* _interface;
    Network*           _network;
    Mutex              _device_lock;

    static MQTT*         _mqtt_handler_ptr;
    const std::size_t    _size;
    char*                _buffer;
    volatile std::size_t _head;
    volatile std::size_t _tail;

    SynchronizableObject<std::pair<mqtt_sta_e, mqtt_server_config_t>> _mqtt_server_config;
    mqtt_pubsub_config_t                                              _mqtt_pubsub_config;
    std::unordered_set<std::string>                                   _sub_topics_set;
};

// Important: due to driver's API, the MQTT handler must be a singleton currently
MQTT* MQTT::_mqtt_handler_ptr = nullptr;

}  // namespace transport

}  // namespace sscma