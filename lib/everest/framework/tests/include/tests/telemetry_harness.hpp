// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <tests/mock_mqtt_abstraction.hpp>
#include <utils/telemetry/consumer.hpp>
#include <utils/telemetry/types.hpp>

/// \file
/// \brief Test harness for the telemetry consumer: an MQTT mock that can deliver, a producer stub,
/// a capturing sink and a catalog builder.
///
/// No broker and no threads. Delivery applies MQTT wildcard semantics, the same rule the framework
/// message handler applies to raw-topic handlers, so a subscription that a test exercises here
/// behaves as it does against a real broker.
namespace Everest::telemetry::tests {

/// \brief MockMQTTAbstraction plus what a test needs from a broker: several handlers per topic
/// pattern, and delivery to the ones a published topic matches.
class BrokerMock : public Everest::tests::MockMQTTAbstraction {
public:
    void register_handler(const std::string& topic, std::shared_ptr<TypedHandler> handler, QOS qos) override {
        Everest::tests::MockMQTTAbstraction::register_handler(topic, handler, qos);
        m_subscriptions.emplace_back(topic, std::move(handler));
    }

    void unregister_handler(const std::string& topic, const Token& token) override {
        Everest::tests::MockMQTTAbstraction::unregister_handler(topic, token);
        for (auto it = m_subscriptions.begin(); it != m_subscriptions.end(); ++it) {
            if (it->first == topic and it->second == token) {
                m_subscriptions.erase(it);
                return;
            }
        }
    }

    /// \brief Delivers \p payload to every subscription whose pattern matches \p topic.
    /// \returns how many handlers received it
    std::size_t deliver(const std::string& topic, const nlohmann::json& payload) {
        auto subscriptions = m_subscriptions; // a handler may unsubscribe while being called
        std::size_t delivered = 0;
        for (const auto& [pattern, handler] : subscriptions) {
            if (topic_matches(pattern, topic)) {
                ++delivered;
                (*handler->handler)(topic, payload);
            }
        }
        return delivered;
    }

    /// \returns the topic patterns currently subscribed, in subscription order
    std::vector<std::string> subscribed_topics() const {
        std::vector<std::string> topics;
        topics.reserve(m_subscriptions.size());
        for (const auto& [pattern, handler] : m_subscriptions) {
            topics.push_back(pattern);
        }
        return topics;
    }

    std::size_t subscription_count() const {
        return m_subscriptions.size();
    }

    /// \brief MQTT topic matching: '+' matches exactly one level, '#' matches the rest.
    static bool topic_matches(std::string_view pattern, std::string_view topic) {
        const auto pattern_levels = split(pattern);
        const auto topic_levels = split(topic);

        for (std::size_t i = 0; i < pattern_levels.size(); ++i) {
            if (pattern_levels[i] == "#") {
                return true;
            }
            if (i >= topic_levels.size()) {
                return false;
            }
            if (pattern_levels[i] == "+") {
                continue;
            }
            if (pattern_levels[i] != topic_levels[i]) {
                return false;
            }
        }
        return pattern_levels.size() == topic_levels.size();
    }

private:
    static std::vector<std::string_view> split(std::string_view topic) {
        std::vector<std::string_view> levels;
        std::size_t start = 0;
        while (true) {
            const auto separator = topic.find('/', start);
            if (separator == std::string_view::npos) {
                levels.push_back(topic.substr(start));
                return levels;
            }
            levels.push_back(topic.substr(start, separator - start));
            start = separator + 1;
        }
    }

    std::vector<std::pair<std::string, std::shared_ptr<TypedHandler>>> m_subscriptions;
};

/// \brief Composes envelopes the way the producer side will, and the only place in the tests that
/// knows the topic scheme.
class PublisherStub {
public:
    /// \brief Everything the framework stamps that a test may want to steer.
    struct Attributes {
        std::optional<std::string> instance;
        std::optional<EvseMapping> mapping;
        std::optional<std::uint64_t> seq;
        std::map<std::string, std::string, std::less<>> labels;
        std::optional<std::string> timestamp;
    };

    PublisherStub(BrokerMock& broker, std::string module_id, std::string module_type) :
        m_broker(broker), m_module_id(std::move(module_id)), m_module_type(std::move(module_type)) {
    }

    /// \brief Publishes a partial update of \p set.
    /// \returns how many subscriptions received it
    std::size_t publish(const std::string& set, const Values& values, Attributes attributes = {}) {
        Envelope envelope;
        envelope.module_id = m_module_id;
        envelope.module_type = m_module_type;
        envelope.set = set;
        envelope.instance = std::move(attributes.instance);
        envelope.mapping = attributes.mapping;
        envelope.seq = attributes.seq;
        envelope.labels = std::move(attributes.labels);
        envelope.timestamp = attributes.timestamp.value_or(next_timestamp());
        envelope.values = values;
        m_last = envelope;
        return m_broker.deliver(topic_for(m_module_id, set), to_payload(envelope));
    }

    /// \brief Publishes a hand-written payload on this producer's topic, for malformed cases.
    std::size_t publish_payload(const std::string& set, const nlohmann::json& payload) {
        return m_broker.deliver(topic_for(m_module_id, set), payload);
    }

    /// \returns the last envelope this stub composed
    const std::optional<Envelope>& last() const {
        return m_last;
    }

    const std::string& module_id() const {
        return m_module_id;
    }

private:
    /// \brief Deterministic RFC3339 timestamps, one second apart, so assertions stay stable.
    std::string next_timestamp() {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "2026-08-12T10:41:%02uZ", static_cast<unsigned>(m_tick++ % 60));
        return std::string{buffer};
    }

    BrokerMock& m_broker;
    std::string m_module_id;
    std::string m_module_type;
    std::optional<Envelope> m_last;
    unsigned m_tick{0};
};

/// \brief Records what one subscriber received.
struct CapturingSink {
    std::vector<Envelope> envelopes;

    Consumer::Callback callback() {
        return [this](const Envelope& envelope) { envelopes.push_back(envelope); };
    }

    std::size_t count() const {
        return envelopes.size();
    }

    bool empty() const {
        return envelopes.empty();
    }

    const Envelope& last() const {
        return envelopes.back();
    }

    void clear() {
        envelopes.clear();
    }
};

/// \brief Builds catalog fixtures without going through YAML.
class DefinitionsBuilder {
public:
    DefinitionsBuilder& module(std::string module_id, std::string module_type) {
        m_module_id = std::move(module_id);
        m_definitions[m_module_id].module_type = std::move(module_type);
        m_set.clear();
        return *this;
    }

    DefinitionsBuilder& set(std::string set, std::optional<double> max_publish_rate_hz = std::nullopt) {
        m_set = std::move(set);
        m_definitions.at(m_module_id).sets[m_set].max_publish_rate_hz = max_publish_rate_hz;
        return *this;
    }

    DefinitionsBuilder& entry(std::string name, EntryDefinition definition) {
        m_definitions.at(m_module_id).sets.at(m_set).entries[std::move(name)] = std::move(definition);
        return *this;
    }

    DefinitionsBuilder& entry(std::string name, EntryType type, std::optional<std::string> unit = std::nullopt) {
        EntryDefinition definition;
        definition.type = type;
        definition.unit = std::move(unit);
        return entry(std::move(name), std::move(definition));
    }

    TelemetryDefinitions build() const {
        return m_definitions;
    }

private:
    TelemetryDefinitions m_definitions;
    std::string m_module_id;
    std::string m_set;
};

/// \brief The powermeter livedata catalog from the proposal, used by several tests.
inline TelemetryDefinitions powermeter_definitions() {
    EntryDefinition temperature;
    temperature.type = EntryType::Number;
    temperature.unit = "Celsius";
    temperature.minimum = -40.0;
    temperature.maximum = 120.0;

    EntryDefinition fw_state;
    fw_state.type = EntryType::String;
    fw_state.values_list = std::vector<std::string>{"Idle", "Measuring", "Error"};

    return DefinitionsBuilder{}
        .module("powermeter_1", "LemDCBM400600")
        .set("livedata", 4.0)
        .entry("temperature_C", temperature)
        .entry("frequency_Hz", EntryType::Number, "Hertz")
        .entry("fw_state", fw_state)
        .build();
}

} // namespace Everest::telemetry::tests
