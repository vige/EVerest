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
/// consumer-side filtering.
///
/// This header deliberately depends on nothing but the standard library and nlohmann/json, so that
/// consumers which are not EVerest modules (a debugging CLI, an out-of-process exporter) can link
/// against the contract without pulling in the framework. Conversions to and from the framework's
/// own types (everest::config::Mapping) live next to the code that needs them.
namespace Everest::telemetry {

/// \brief Envelope version stamped by the framework, independent of the module's own data.
constexpr int ENVELOPE_VERSION = 1;

/// \brief Topic prefix of the value flow. The two segments that follow are module id and set.
constexpr std::string_view TOPIC_PREFIX = "everest-telemetry/v1/";

/// \brief The single subscription a consumer needs to see the whole value flow.
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
    std::optional<std::string> unit;                       ///< -> VariableCharacteristics.unit
    std::optional<std::string> description;                ///< human readable, not sent per sample
    std::optional<std::vector<std::string>> values_list;   ///< -> VariableCharacteristics.valuesList
    std::optional<double> minimum;                         ///< -> VariableCharacteristics.minLimit
    std::optional<double> maximum;                         ///< -> VariableCharacteristics.maxLimit
};

/// \brief A declared telemetry set: the unit of publishing and of subscription.
struct SetDefinition {
    std::optional<std::string> description;
    std::map<std::string, EntryDefinition, std::less<>> entries;
    std::optional<double> max_publish_rate_hz; ///< producer-side rate limit, enforced by the framework
};

/// \brief All telemetry declared by one module instance.
struct ModuleDefinition {
    std::string module_type; ///< module type, so a consumer can filter without seeing a sample first
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
/// Mirrors everest::config::Mapping on purpose: telemetry addressing must not force the contract
/// header to depend on the framework configuration types.
struct Mapping {
    int evse{0};
    std::optional<int> connector;
};

/// \brief What a telemetry value flow is addressed by.
///
/// module_id and set come from the declaration. instance names one device inside a module that
/// handles several of them: 24 power converters behind one driver, two 4G modems behind one modem
/// module. Such devices are not distinguishable by EVSE id, and in a station with dynamic power
/// allocation they are not EVSE-specific at all.
struct Address {
    std::string module_id;
    std::string set;
    std::optional<std::string> instance;

    /// \brief Total order, so an Address can key a cache or a dispatch table.
    bool operator<(const Address& other) const;
    bool operator==(const Address& other) const;

    /// \returns "module_id/set" or "module_id/set/instance", for logs and diagnostics
    std::string to_string() const;
};

// ---------------------------------------------------------------------------
// Values and envelope
// ---------------------------------------------------------------------------

/// \brief One telemetry value. A consumer is generic by construction: it learns the declared type
/// from the catalog at runtime instead of compiling against per-entry types, which is the deliberate
/// asymmetry to the producer side (typed structs generated from the module's own manifest).
using Value = std::variant<bool, std::int64_t, double, std::string, nlohmann::json>;

/// \brief Entry name to value. Partial by design: an envelope carries what changed.
using Values = std::map<std::string, Value, std::less<>>;

/// \returns the EntryType a \p value would satisfy exactly (Integer for integral, Number for
/// floating point, Object/Array for structured json)
EntryType type_of(const Value& value);

/// \brief One telemetry message. Everything but the set id and the values is stamped by the
/// framework at publish time.
struct Envelope {
    int version{ENVELOPE_VERSION};
    std::string module_id;
    std::string module_type;
    std::string set;
    std::optional<std::string> instance;
    std::string timestamp; ///< RFC3339, injected by the framework
    std::optional<Mapping> mapping;
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

/// \brief Consumer-side subscription filter. Every unset member matches anything.
///
/// Filters are matched in process against a single broker subscription, never turned into broker
/// subscriptions of their own.
struct Filter {
    std::optional<std::string> module_id;
    std::optional<std::string> module_type;
    std::optional<std::string> set;
    std::optional<std::string> instance;
    std::optional<int> evse;      ///< requires a mapping with this EVSE id
    std::optional<int> connector; ///< requires a mapping with this connector id

    bool matches(const Envelope& envelope) const;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/// \brief Why an incoming payload could not be turned into an Envelope.
enum class ParseError {
    None,
    NotAnObject,        ///< payload is not a json object
    NotJson,            ///< payload is not valid json at all
    UnsupportedVersion, ///< envelope version this consumer does not understand
    MissingField,       ///< a required envelope field is absent
    WrongFieldType,     ///< a required envelope field has the wrong json type
    TopicMismatch,      ///< topic and payload disagree on module id or set
    NotATelemetryTopic  ///< topic is not part of the value flow
};

std::string_view to_string(ParseError error);

/// \brief Outcome of parsing one payload.
struct ParseResult {
    ParseError error{ParseError::None};
    std::string message; ///< which field, for logs; never used for control flow
    std::optional<Envelope> envelope;

    bool ok() const {
        return error == ParseError::None and envelope.has_value();
    }
};

/// \brief Parses \p payload received on \p topic into an Envelope.
///
/// Unknown envelope fields are ignored, so a newer producer within the same envelope version stays
/// readable. An empty values object is accepted: it carries the address, timestamp and seq.
ParseResult parse_envelope(std::string_view topic, const nlohmann::json& payload);

/// \brief Parses raw \p payload bytes received on \p topic. Invalid json yields ParseError::NotJson.
ParseResult parse_envelope(std::string_view topic, std::string_view payload);

// ---------------------------------------------------------------------------
// Validation against the catalog
// ---------------------------------------------------------------------------

/// \brief What is wrong with one value, measured against its declaration.
enum class ValueIssue {
    UndeclaredModule,
    UndeclaredSet,
    UndeclaredEntry,
    TypeMismatch,
    OutOfRange,
    NotInValuesList
};

std::string_view to_string(ValueIssue issue);

/// \brief One finding. entry is empty for issues that concern the whole envelope.
struct ValidationFinding {
    std::string entry;
    ValueIssue issue;
};

struct ValidationResult {
    std::vector<ValidationFinding> findings;

    bool ok() const {
        return findings.empty();
    }
};

/// \brief Checks every value in \p envelope against \p definitions.
///
/// An Integer declaration accepts only integral values; a Number declaration also accepts integral
/// ones, because json does not distinguish 42 from 42.0. minimum/maximum and values_list are
/// checked where declared.
ValidationResult validate(const Envelope& envelope, const TelemetryDefinitions& definitions);

// ---------------------------------------------------------------------------
// json conversions
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& j, const EntryDefinition& definition);
void from_json(const nlohmann::json& j, EntryDefinition& definition);
void to_json(nlohmann::json& j, const SetDefinition& definition);
void from_json(const nlohmann::json& j, SetDefinition& definition);
void to_json(nlohmann::json& j, const ModuleDefinition& definition);
void from_json(const nlohmann::json& j, ModuleDefinition& definition);
void to_json(nlohmann::json& j, const Mapping& mapping);
void from_json(const nlohmann::json& j, Mapping& mapping);
/// \brief Serializes \p envelope the way the producer side puts it on the wire.
nlohmann::json to_payload(const Envelope& envelope);

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
