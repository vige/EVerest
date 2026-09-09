// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/consumer.hpp>

#include <utility>

namespace Everest::telemetry {

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

Consumer::Subscription::Subscription(MQTTAbstraction& mqtt, std::string topic, Token token) :
    m_mqtt(&mqtt), m_topic(std::move(topic)), m_token(std::move(token)) {
}

Consumer::Subscription::Subscription(Subscription&& other) noexcept :
    m_mqtt(other.m_mqtt), m_topic(std::move(other.m_topic)), m_token(std::move(other.m_token)) {
    other.m_mqtt = nullptr;
    other.m_token = nullptr;
}

Consumer::Subscription& Consumer::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        m_mqtt = other.m_mqtt;
        m_topic = std::move(other.m_topic);
        m_token = std::move(other.m_token);
        other.m_mqtt = nullptr;
        other.m_token = nullptr;
    }
    return *this;
}

Consumer::Subscription::~Subscription() {
    reset();
}

void Consumer::Subscription::reset() {
    if (m_token == nullptr) {
        return;
    }
    m_mqtt->unregister_handler(m_topic, m_token);
    m_token = nullptr;
    m_mqtt = nullptr;
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------

Consumer::Consumer(MQTTAbstraction& mqtt) : m_mqtt(mqtt) {
}

Consumer::Subscription Consumer::subscribe(const Filter& filter, Callback callback) {
    auto topic = filter.topic_pattern();

    auto handler = std::make_shared<Handler>([callback = std::move(callback)](const std::string& topic,
                                                                             json data) {
        // The framework only decodes payloads on topics under its own prefix; everything else
        // reaches a raw-topic handler as the undecoded payload wrapped in a json string. The
        // telemetry flow lives outside that prefix, so the payload is decoded here.
        if (data.is_string()) {
            try {
                data = json::parse(data.get_ref<const std::string&>());
            } catch (const json::parse_error&) {
                return;
            }
        }

        if (auto envelope = read_envelope(topic, data); envelope.has_value()) {
            callback(*envelope);
        }
    });

    // ExternalMQTT handlers are the framework's raw-topic handlers: they are matched against the
    // incoming topic with MQTT wildcard semantics, which is what makes a filter subscription work.
    auto token = std::make_shared<TypedHandler>(topic, HandlerType::ExternalMQTT, std::move(handler));
    m_mqtt.register_handler(topic, token, QOS::QOS0);

    return Subscription{m_mqtt, std::move(topic), std::move(token)};
}

void Consumer::set_definitions(TelemetryDefinitions definitions) {
    m_definitions = std::move(definitions);
}

const TelemetryDefinitions& Consumer::definitions() const {
    return m_definitions;
}

} // namespace Everest::telemetry
