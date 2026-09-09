// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

/// \file
/// \brief Wire contract of the EVerest telemetry feature: declaration catalog, value envelope and
/// the subscription filter.
///
/// This header depends on nothing but the standard library and nlohmann/json, so a consumer which
/// is not an EVerest module (a debugging CLI, an out-of-process exporter) can link against the
/// contract without the framework.
///
/// Values are not validated here. The producer declares its telemetry in its manifest and the
/// framework checks what it publishes against that declaration, so a consumer takes the values as
/// they arrive and only ignores a payload it cannot read at all.
namespace Everest::telemetry {

/// \brief Envelope version stamped by the framework, independent of the module's own data.
constexpr int ENVELOPE_VERSION = 1;

/// \brief Topic prefix of the value flow. The two segments that follow are module id and set.
constexpr std::string_view TOPIC_PREFIX = "everest-telemetry/v1/";

/// \brief The topic a consumer subscribes to when it filters on nothing.
constexpr std::string_view TOPIC_WILDCARD = "everest-telemetry/v1/+/+";

// ---------------------------------------------------------------------------
// Declaration catalog
// ---------------------------------------------------------------------------

/// \brief The declared type of a telemetry entry. Fixed vocabulary of the manifest declaration.
enum class EntryType {
    Boolean,
    Integer,
    Number,
    String,
    Object,
    Array
};

/// \returns the manifest spelling of \p type ("boolean", "integer", ...)
std::string_view to_string(EntryType type);

/// \returns the EntryType for the manifest spelling \p name, or nothing when \p name is not part of
/// the vocabulary
std::optional<EntryType> entry_type_from_string(std::string_view name);

/// \brief One declared telemetry entry. The optional metadata covers what OCPP
/// VariableCharacteristics needs.
struct EntryDefinition {
    EntryType type{EntryType::String};
    std::optional<std::string> unit;                     ///< -> VariableCharacteristics.unit
    std::optional<std::string> description;              ///< human readable, not sent per sample
    std::optional<std::vector<std::string>> values_list; ///< -> VariableCharacteristics.valuesList
    std::optional<double> minimum;                       ///< -> VariableCharacteristics.minLimit
    std::optional<double> maximum;                       ///< -> VariableCharacteristics.maxLimit
};

/// \brief A declared telemetry set: the unit of publishing and of subscription.
struct SetDefinition {
    std::optional<std::string> description;
    std::map<std::string, EntryDefinition, std::less<>> entries;
    std::optional<double> max_publish_rate_hz; ///< producer-side rate limit, enforced by the framework
};

/// \brief All telemetry declared by one module instance.
struct ModuleDefinition {
    std::string module_type; ///< module type, known before a sample has been seen
    std::map<std::string, SetDefinition, std::less<>> sets;
};

/// \brief The full station catalog, keyed by module id.
///
/// Shaped so that the lookup reads as it does in the proposal:
/// \code
/// definitions.at("powermeter_1").sets.at("livedata").entries.at("temperature_C")
/// \endcode
using TelemetryDefinitions = std::map<std::string, ModuleDefinition, std::less<>>;

/// \returns the declaration of one entry, or nullptr when any level of the path is undeclared
const EntryDefinition* find_entry(const TelemetryDefinitions& definitions, std::string_view module_id,
                                  std::string_view set, std::string_view entry);

// ---------------------------------------------------------------------------
// Addressing
// ---------------------------------------------------------------------------

/// \brief EVSE and connector attribution, taken from the module's standard mapping.
///
/// Mirrors everest::config::Mapping on purpose, so the contract header stays free of the framework
/// configuration types.
struct EvseMapping {
    int evse{0};
    std::optional<int> connector;
};

/// \brief What a telemetry value flow is addressed by.
///
/// module_id and set come from the topic. instance names one device inside a module that handles
/// several of them: 24 power converters behind one driver, two modems behind one modem module. Such
/// devices are not distinguishable by EVSE id, and with dynamic power allocation they are not
/// EVSE-specific at all. It travels in the payload, not the topic.
struct Address {
    std::string module_id;
    std::string set;
    std::optional<std::string> instance;

    /// \brief Total order, so an Address can key a cache.
    bool operator<(const Address& other) const;
    bool operator==(const Address& other) const;

    /// \returns "module_id/set" or "module_id/set/instance", for logs and diagnostics
    std::string to_string() const;
};

// ---------------------------------------------------------------------------
// Values and envelope
// ---------------------------------------------------------------------------

/// \brief One telemetry value. A consumer is generic by construction: it reads the declared type
/// from the catalog at runtime instead of compiling against per-entry types, which is the deliberate
/// asymmetry to the producer side (typed structs generated from the module's own manifest).
using Value = std::variant<bool, std::int64_t, double, std::string, nlohmann::json>;

/// \brief Entry name to value. Partial by design: an envelope carries what changed.
using Values = std::map<std::string, Value, std::less<>>;

/// \returns the EntryType a \p value holds
EntryType type_of(const Value& value);

/// \brief One telemetry message. Everything but the set id and the values is stamped by the
/// framework at publish time.
struct Envelope {
    int version{ENVELOPE_VERSION};
    std::string module_id;   ///< from the topic
    std::string set;         ///< from the topic
    std::string module_type; ///< from the payload
    std::optional<std::string> instance;
    std::string timestamp; ///< RFC3339, injected by the framework
    std::optional<EvseMapping> mapping;
    std::optional<std::uint64_t> seq; ///< monotonic per address, lets a consumer detect QoS 0 loss
    std::map<std::string, std::string, std::less<>> labels; ///< producer-defined dimensions
    Values values;

    Address address() const;
};

/// \returns the value flow topic for \p module_id and \p set
std::string topic_for(std::string_view module_id, std::string_view set);

/// \brief The module id and set encoded in a value flow topic.
struct TopicAddress {
    std::string module_id;
    std::string set;
};

/// \returns the address encoded in \p topic, or nothing when \p topic is not a value flow topic
std::optional<TopicAddress> parse_topic(std::string_view topic);

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

/// \brief What a consumer wants to receive. The filter is the broker subscription: each unset
/// member becomes a single-level wildcard, so the broker delivers only matching topics and nothing
/// is filtered again in process.
///
/// Only what the topic carries can be filtered. The set is the one that matters - it separates fast
/// livedata from slow diagnostics - and module id addresses one producer.
struct Filter {
    std::optional<std::string> module_id;
    std::optional<std::string> set;

    /// \returns the MQTT topic to subscribe to, for example
    /// "everest-telemetry/v1/+/livedata" for a filter on the set alone
    std::string topic_pattern() const;
};

// ---------------------------------------------------------------------------
// Reading a payload
// ---------------------------------------------------------------------------

/// \brief Reads the envelope for a message received on \p topic.
///
/// The topic is authoritative for module id and set; the payload carries the rest. Unknown payload
/// fields are ignored, so a newer producer within the same envelope version stays readable.
///
/// \returns nothing when \p topic is not a value flow topic, the payload is not an object, its
/// version is not understood, or it carries no values object. A consumer ignores such a message: the
/// producer side is where a declaration is enforced.
std::optional<Envelope> read_envelope(std::string_view topic, const nlohmann::json& payload);

/// \brief Serializes \p envelope the way the producer side puts it on the wire. Module id and set
/// are carried by the topic, not repeated in the payload.
nlohmann::json to_payload(const Envelope& envelope);

// ---------------------------------------------------------------------------
// json conversions
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& j, const EntryDefinition& definition);
void from_json(const nlohmann::json& j, EntryDefinition& definition);
void to_json(nlohmann::json& j, const SetDefinition& definition);
void from_json(const nlohmann::json& j, SetDefinition& definition);
void to_json(nlohmann::json& j, const ModuleDefinition& definition);
void from_json(const nlohmann::json& j, ModuleDefinition& definition);
void to_json(nlohmann::json& j, const EvseMapping& mapping);
void from_json(const nlohmann::json& j, EvseMapping& mapping);

} // namespace Everest::telemetry

namespace nlohmann {

/// \brief Value is a std::variant, whose converting constructor would make an ADL to_json() overload
/// a candidate for every type convertible to it. The serializer is specialized instead, following
/// the framework's own convention for the config types.
template <> struct adl_serializer<Everest::telemetry::Value> {
    static void to_json(json& j, const Everest::telemetry::Value& value);
    static void from_json(const json& j, Everest::telemetry::Value& value);
};

} // namespace nlohmann
