// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <utils/telemetry/consumer_core.hpp>
#include <utils/telemetry/types.hpp>

/// \file
/// \brief Test harness for the telemetry consumer: a fake bus with MQTT topic semantics, a producer
/// stub, a capturing sink and a catalog builder.
///
/// No broker and no threads. The fake bus routes on the same wildcard rules the real broker applies,
/// so a test written against it keeps its meaning once the MQTT binding replaces the bus.
namespace Everest::telemetry::tests {

/// \brief In-memory stand-in for the broker, with MQTT single-level wildcard routing.
class FakeBus {
public:
    using Handler = std::function<void(const std::string& topic, const nlohmann::json& payload)>;

    /// \brief Registers \p handler for a topic pattern, which may contain '+' and a trailing '#'.
    void register_handler(std::string pattern, Handler handler) {
        m_handlers.emplace_back(std::move(pattern), std::move(handler));
    }

    /// \brief Delivers \p payload to every handler whose pattern matches \p topic.
    /// \returns how many handlers received it
    std::size_t publish(const std::string& topic, const nlohmann::json& payload) {
        ++m_published;
        std::size_t delivered = 0;
        for (const auto& [pattern, handler] : m_handlers) {
            if (topic_matches(pattern, topic)) {
                ++delivered;
                handler(topic, payload);
            }
        }
        m_delivered += delivered;
        return delivered;
    }

    /// \brief Delivers a raw payload, for cases the producer stub cannot express (invalid json).
    std::size_t publish_raw(const std::string& topic, std::string_view payload) {
        ++m_published;
        const auto parsed = nlohmann::json::parse(payload, nullptr, false);
        std::size_t delivered = 0;
        for (const auto& [pattern, handler] : m_handlers) {
            if (topic_matches(pattern, topic)) {
                ++delivered;
                // A real broker hands over bytes; a discarded parse stands for "not json".
                handler(topic, parsed);
            }
        }
        m_delivered += delivered;
        return delivered;
    }

    std::size_t published() const {
        return m_published;
    }

    std::size_t delivered() const {
        return m_delivered;
    }

    std::size_t handler_count() const {
        return m_handlers.size();
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

    std::vector<std::pair<std::string, Handler>> m_handlers;
    std::size_t m_published{0};
    std::size_t m_delivered{0};
};

/// \brief Composes envelopes the way the producer side will, and the only place in the tests that
/// knows the topic scheme.
class PublisherStub {
public:
    /// \brief Everything the framework stamps that a test may want to steer.
    struct Attributes {
        std::optional<std::string> instance;
        std::optional<Mapping> mapping;
        std::optional<std::uint64_t> seq;
        std::map<std::string, std::string, std::less<>> labels;
        std::optional<std::string> timestamp;
    };

    PublisherStub(FakeBus& bus, std::string module_id, std::string module_type) :
        m_bus(bus), m_module_id(std::move(module_id)), m_module_type(std::move(module_type)) {
    }

    /// \brief Publishes a partial update of \p set.
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
        return m_bus.publish(topic_for(m_module_id, set), to_payload(envelope));
    }

    /// \brief Publishes a hand-written payload on this producer's topic, for malformed cases.
    std::size_t publish_payload(const std::string& set, const nlohmann::json& payload) {
        return m_bus.publish(topic_for(m_module_id, set), payload);
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

    FakeBus& m_bus;
    std::string m_module_id;
    std::string m_module_type;
    std::optional<Envelope> m_last;
    unsigned m_tick{0};
};

/// \brief Records what one subscriber received.
struct CapturingSink {
    std::vector<Envelope> envelopes;

    ConsumerCore::Callback callback() {
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

/// \brief Wires \p core to \p bus exactly as the MQTT binding will: one wildcard subscription.
inline void connect(ConsumerCore& core, FakeBus& bus) {
    bus.register_handler(std::string{ConsumerCore::wildcard_topic()},
                         [&core](const std::string& topic, const nlohmann::json& payload) {
                             core.handle_message(topic, payload);
                         });
}

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
