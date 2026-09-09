// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/types.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace Everest::telemetry {

namespace {

/// \brief Reads a required string field, reporting which field was at fault.
bool read_string(const nlohmann::json& payload, const char* field, std::string& out, ParseResult& result) {
    const auto it = payload.find(field);
    if (it == payload.end()) {
        result.error = ParseError::MissingField;
        result.message = field;
        return false;
    }
    if (not it->is_string()) {
        result.error = ParseError::WrongFieldType;
        result.message = field;
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/// \returns the numeric value of \p value for range checks, or nothing for non-numeric values
std::optional<double> as_number(const Value& value) {
    if (std::holds_alternative<std::int64_t>(value)) {
        return static_cast<double>(std::get<std::int64_t>(value));
    }
    if (std::holds_alternative<double>(value)) {
        return std::get<double>(value);
    }
    return std::nullopt;
}

bool type_matches(EntryType declared, const Value& value) {
    const auto actual = type_of(value);
    if (declared == actual) {
        return true;
    }
    // json does not distinguish 42 from 42.0, so a Number declaration accepts integral values.
    return declared == EntryType::Number and actual == EntryType::Integer;
}

} // namespace

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

std::string_view to_string(EntryType type) {
    switch (type) {
    case EntryType::Boolean:
        return "boolean";
    case EntryType::Integer:
        return "integer";
    case EntryType::Number:
        return "number";
    case EntryType::String:
        return "string";
    case EntryType::Object:
        return "object";
    case EntryType::Array:
        return "array";
    }
    return "unknown";
}

std::optional<EntryType> entry_type_from_string(std::string_view name) {
    if (name == "boolean") {
        return EntryType::Boolean;
    }
    if (name == "integer") {
        return EntryType::Integer;
    }
    if (name == "number") {
        return EntryType::Number;
    }
    if (name == "string") {
        return EntryType::String;
    }
    if (name == "object") {
        return EntryType::Object;
    }
    if (name == "array") {
        return EntryType::Array;
    }
    return std::nullopt;
}

std::string_view to_string(ParseError error) {
    switch (error) {
    case ParseError::None:
        return "none";
    case ParseError::NotAnObject:
        return "not an object";
    case ParseError::NotJson:
        return "not json";
    case ParseError::UnsupportedVersion:
        return "unsupported version";
    case ParseError::MissingField:
        return "missing field";
    case ParseError::WrongFieldType:
        return "wrong field type";
    case ParseError::TopicMismatch:
        return "topic mismatch";
    case ParseError::NotATelemetryTopic:
        return "not a telemetry topic";
    }
    return "unknown";
}

std::string_view to_string(ValueIssue issue) {
    switch (issue) {
    case ValueIssue::UndeclaredModule:
        return "undeclared module";
    case ValueIssue::UndeclaredSet:
        return "undeclared set";
    case ValueIssue::UndeclaredEntry:
        return "undeclared entry";
    case ValueIssue::TypeMismatch:
        return "type mismatch";
    case ValueIssue::OutOfRange:
        return "out of range";
    case ValueIssue::NotInValuesList:
        return "not in values list";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

const EntryDefinition* find_entry(const TelemetryDefinitions& definitions, std::string_view module_id,
                                  std::string_view set, std::string_view entry) {
    const auto module_it = definitions.find(module_id);
    if (module_it == definitions.end()) {
        return nullptr;
    }
    const auto set_it = module_it->second.sets.find(set);
    if (set_it == module_it->second.sets.end()) {
        return nullptr;
    }
    const auto entry_it = set_it->second.entries.find(entry);
    if (entry_it == set_it->second.entries.end()) {
        return nullptr;
    }
    return &entry_it->second;
}

// ---------------------------------------------------------------------------
// Addressing
// ---------------------------------------------------------------------------

bool Address::operator<(const Address& other) const {
    return std::tie(module_id, set, instance) < std::tie(other.module_id, other.set, other.instance);
}

bool Address::operator==(const Address& other) const {
    return std::tie(module_id, set, instance) == std::tie(other.module_id, other.set, other.instance);
}

std::string Address::to_string() const {
    auto out = module_id + "/" + set;
    if (instance.has_value()) {
        out += "/" + *instance;
    }
    return out;
}

Address Envelope::address() const {
    return Address{module_id, set, instance};
}

std::string topic_for(std::string_view module_id, std::string_view set) {
    std::string topic{TOPIC_PREFIX};
    topic.append(module_id);
    topic.push_back('/');
    topic.append(set);
    return topic;
}

std::optional<TopicAddress> parse_topic(std::string_view topic) {
    if (topic.substr(0, TOPIC_PREFIX.size()) != TOPIC_PREFIX) {
        return std::nullopt;
    }
    const auto tail = topic.substr(TOPIC_PREFIX.size());
    const auto separator = tail.find('/');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto module_id = tail.substr(0, separator);
    const auto set = tail.substr(separator + 1);
    // Exactly two segments: a deeper topic is not part of this contract.
    if (module_id.empty() or set.empty() or set.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return TopicAddress{std::string{module_id}, std::string{set}};
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

EntryType type_of(const Value& value) {
    if (std::holds_alternative<bool>(value)) {
        return EntryType::Boolean;
    }
    if (std::holds_alternative<std::int64_t>(value)) {
        return EntryType::Integer;
    }
    if (std::holds_alternative<double>(value)) {
        return EntryType::Number;
    }
    if (std::holds_alternative<std::string>(value)) {
        return EntryType::String;
    }
    return std::get<nlohmann::json>(value).is_array() ? EntryType::Array : EntryType::Object;
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

bool Filter::matches(const Envelope& envelope) const {
    if (module_id.has_value() and *module_id != envelope.module_id) {
        return false;
    }
    if (module_type.has_value() and *module_type != envelope.module_type) {
        return false;
    }
    if (set.has_value() and *set != envelope.set) {
        return false;
    }
    if (instance.has_value() and instance != envelope.instance) {
        return false;
    }
    if (evse.has_value()) {
        if (not envelope.mapping.has_value() or envelope.mapping->evse != *evse) {
            return false;
        }
    }
    if (connector.has_value()) {
        if (not envelope.mapping.has_value() or envelope.mapping->connector != connector) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// json conversions
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& j, const EntryDefinition& definition) {
    j = nlohmann::json::object();
    j["type"] = std::string{to_string(definition.type)};
    if (definition.unit.has_value()) {
        j["unit"] = *definition.unit;
    }
    if (definition.description.has_value()) {
        j["description"] = *definition.description;
    }
    if (definition.values_list.has_value()) {
        j["values_list"] = *definition.values_list;
    }
    if (definition.minimum.has_value()) {
        j["minimum"] = *definition.minimum;
    }
    if (definition.maximum.has_value()) {
        j["maximum"] = *definition.maximum;
    }
}

void from_json(const nlohmann::json& j, EntryDefinition& definition) {
    const auto type = entry_type_from_string(j.at("type").get<std::string>());
    if (not type.has_value()) {
        throw std::out_of_range("unknown telemetry entry type: " + j.at("type").get<std::string>());
    }
    definition.type = *type;
    if (const auto it = j.find("unit"); it != j.end()) {
        definition.unit = it->get<std::string>();
    }
    if (const auto it = j.find("description"); it != j.end()) {
        definition.description = it->get<std::string>();
    }
    if (const auto it = j.find("values_list"); it != j.end()) {
        definition.values_list = it->get<std::vector<std::string>>();
    }
    if (const auto it = j.find("minimum"); it != j.end()) {
        definition.minimum = it->get<double>();
    }
    if (const auto it = j.find("maximum"); it != j.end()) {
        definition.maximum = it->get<double>();
    }
}

void to_json(nlohmann::json& j, const SetDefinition& definition) {
    j = nlohmann::json::object();
    if (definition.description.has_value()) {
        j["description"] = *definition.description;
    }
    auto entries = nlohmann::json::object();
    for (const auto& [name, entry] : definition.entries) {
        entries[name] = entry;
    }
    j["entries"] = std::move(entries);
    if (definition.max_publish_rate_hz.has_value()) {
        j["max_publish_rate_hz"] = *definition.max_publish_rate_hz;
    }
}

void from_json(const nlohmann::json& j, SetDefinition& definition) {
    if (const auto it = j.find("description"); it != j.end()) {
        definition.description = it->get<std::string>();
    }
    const auto entries = j.find("entries");
    if (entries != j.end()) {
        for (const auto& [name, entry] : entries->items()) {
            definition.entries[name] = entry.get<EntryDefinition>();
        }
    }
    if (const auto it = j.find("max_publish_rate_hz"); it != j.end()) {
        definition.max_publish_rate_hz = it->get<double>();
    }
}

void to_json(nlohmann::json& j, const ModuleDefinition& definition) {
    j = nlohmann::json::object();
    j["module_type"] = definition.module_type;
    auto sets = nlohmann::json::object();
    for (const auto& [name, set] : definition.sets) {
        sets[name] = set;
    }
    j["sets"] = std::move(sets);
}

void from_json(const nlohmann::json& j, ModuleDefinition& definition) {
    definition.module_type = j.at("module_type").get<std::string>();
    const auto sets = j.find("sets");
    if (sets != j.end()) {
        for (const auto& [name, set] : sets->items()) {
            definition.sets[name] = set.get<SetDefinition>();
        }
    }
}

void to_json(nlohmann::json& j, const Mapping& mapping) {
    j = nlohmann::json::object();
    j["evse"] = mapping.evse;
    if (mapping.connector.has_value()) {
        j["connector"] = *mapping.connector;
    }
}

void from_json(const nlohmann::json& j, Mapping& mapping) {
    mapping.evse = j.at("evse").get<int>();
    if (const auto it = j.find("connector"); it != j.end()) {
        mapping.connector = it->get<int>();
    }
}

nlohmann::json to_payload(const Envelope& envelope) {
    auto payload = nlohmann::json::object();
    payload["version"] = envelope.version;
    payload["module_id"] = envelope.module_id;
    payload["module_type"] = envelope.module_type;
    payload["set"] = envelope.set;
    if (envelope.instance.has_value()) {
        payload["instance"] = *envelope.instance;
    }
    payload["timestamp"] = envelope.timestamp;
    if (envelope.mapping.has_value()) {
        payload["mapping"] = *envelope.mapping;
    }
    if (envelope.seq.has_value()) {
        payload["seq"] = *envelope.seq;
    }
    if (not envelope.labels.empty()) {
        auto labels = nlohmann::json::object();
        for (const auto& [key, label] : envelope.labels) {
            labels[key] = label;
        }
        payload["labels"] = std::move(labels);
    }
    auto values = nlohmann::json::object();
    for (const auto& [name, value] : envelope.values) {
        values[name] = value;
    }
    payload["values"] = std::move(values);
    return payload;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

ParseResult parse_envelope(std::string_view topic, const nlohmann::json& payload) {
    ParseResult result;

    const auto topic_address = parse_topic(topic);
    if (not topic_address.has_value()) {
        result.error = ParseError::NotATelemetryTopic;
        result.message = std::string{topic};
        return result;
    }

    if (not payload.is_object()) {
        result.error = ParseError::NotAnObject;
        return result;
    }

    Envelope envelope;

    const auto version = payload.find("version");
    if (version == payload.end()) {
        result.error = ParseError::MissingField;
        result.message = "version";
        return result;
    }
    if (not version->is_number_integer()) {
        result.error = ParseError::WrongFieldType;
        result.message = "version";
        return result;
    }
    envelope.version = version->get<int>();
    if (envelope.version != ENVELOPE_VERSION) {
        result.error = ParseError::UnsupportedVersion;
        result.message = std::to_string(envelope.version);
        return result;
    }

    if (not read_string(payload, "module_id", envelope.module_id, result) or
        not read_string(payload, "module_type", envelope.module_type, result) or
        not read_string(payload, "set", envelope.set, result) or
        not read_string(payload, "timestamp", envelope.timestamp, result)) {
        return result;
    }

    if (envelope.module_id != topic_address->module_id or envelope.set != topic_address->set) {
        result.error = ParseError::TopicMismatch;
        result.message = topic_for(envelope.module_id, envelope.set);
        return result;
    }

    if (const auto it = payload.find("instance"); it != payload.end()) {
        if (not it->is_string()) {
            result.error = ParseError::WrongFieldType;
            result.message = "instance";
            return result;
        }
        envelope.instance = it->get<std::string>();
    }

    if (const auto it = payload.find("mapping"); it != payload.end()) {
        if (not it->is_object() or not it->contains("evse") or not it->at("evse").is_number_integer()) {
            result.error = ParseError::WrongFieldType;
            result.message = "mapping";
            return result;
        }
        envelope.mapping = it->get<Mapping>();
    }

    if (const auto it = payload.find("seq"); it != payload.end()) {
        if (not it->is_number_unsigned()) {
            result.error = ParseError::WrongFieldType;
            result.message = "seq";
            return result;
        }
        envelope.seq = it->get<std::uint64_t>();
    }

    if (const auto it = payload.find("labels"); it != payload.end()) {
        if (not it->is_object()) {
            result.error = ParseError::WrongFieldType;
            result.message = "labels";
            return result;
        }
        for (const auto& [key, label] : it->items()) {
            if (not label.is_string()) {
                result.error = ParseError::WrongFieldType;
                result.message = "labels." + key;
                return result;
            }
            envelope.labels[key] = label.get<std::string>();
        }
    }

    const auto values = payload.find("values");
    if (values == payload.end()) {
        result.error = ParseError::MissingField;
        result.message = "values";
        return result;
    }
    if (not values->is_object()) {
        result.error = ParseError::WrongFieldType;
        result.message = "values";
        return result;
    }
    for (const auto& [name, value] : values->items()) {
        envelope.values[name] = value.get<Value>();
    }

    result.envelope = std::move(envelope);
    return result;
}

ParseResult parse_envelope(std::string_view topic, std::string_view payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) {
        ParseResult result;
        result.error = ParseError::NotJson;
        return result;
    }
    return parse_envelope(topic, parsed);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

ValidationResult validate(const Envelope& envelope, const TelemetryDefinitions& definitions) {
    ValidationResult result;

    const auto module_it = definitions.find(envelope.module_id);
    if (module_it == definitions.end()) {
        result.findings.push_back({"", ValueIssue::UndeclaredModule});
        return result;
    }
    const auto set_it = module_it->second.sets.find(envelope.set);
    if (set_it == module_it->second.sets.end()) {
        result.findings.push_back({"", ValueIssue::UndeclaredSet});
        return result;
    }

    for (const auto& [name, value] : envelope.values) {
        const auto entry_it = set_it->second.entries.find(name);
        if (entry_it == set_it->second.entries.end()) {
            result.findings.push_back({name, ValueIssue::UndeclaredEntry});
            continue;
        }
        const auto& declaration = entry_it->second;
        if (not type_matches(declaration.type, value)) {
            result.findings.push_back({name, ValueIssue::TypeMismatch});
            continue;
        }
        if (const auto number = as_number(value); number.has_value()) {
            if ((declaration.minimum.has_value() and *number < *declaration.minimum) or
                (declaration.maximum.has_value() and *number > *declaration.maximum)) {
                result.findings.push_back({name, ValueIssue::OutOfRange});
                continue;
            }
        }
        if (declaration.values_list.has_value() and std::holds_alternative<std::string>(value)) {
            const auto& allowed = *declaration.values_list;
            const auto& text = std::get<std::string>(value);
            if (std::find(allowed.begin(), allowed.end(), text) == allowed.end()) {
                result.findings.push_back({name, ValueIssue::NotInValuesList});
            }
        }
    }

    return result;
}

} // namespace Everest::telemetry

namespace nlohmann {

void adl_serializer<Everest::telemetry::Value>::to_json(json& j, const Everest::telemetry::Value& value) {
    std::visit([&j](const auto& held) { j = held; }, value);
}

void adl_serializer<Everest::telemetry::Value>::from_json(const json& j, Everest::telemetry::Value& value) {
    if (j.is_boolean()) {
        value = j.get<bool>();
    } else if (j.is_number_integer() or j.is_number_unsigned()) {
        value = j.get<std::int64_t>();
    } else if (j.is_number_float()) {
        value = j.get<double>();
    } else {
        value = j.is_string() ? Everest::telemetry::Value{j.get<std::string>()} : Everest::telemetry::Value{j};
    }
}

} // namespace nlohmann
