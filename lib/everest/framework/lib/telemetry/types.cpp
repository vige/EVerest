// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/types.hpp>

#include <tuple>
#include <utility>

namespace Everest::telemetry {

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

std::string Filter::topic_pattern() const {
    return topic_for(module_id.value_or("+"), set.value_or("+"));
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

void to_json(nlohmann::json& j, const EvseMapping& mapping) {
    j = nlohmann::json::object();
    j["evse"] = mapping.evse;
    if (mapping.connector.has_value()) {
        j["connector"] = *mapping.connector;
    }
}

void from_json(const nlohmann::json& j, EvseMapping& mapping) {
    mapping.evse = j.at("evse").get<int>();
    if (const auto it = j.find("connector"); it != j.end()) {
        mapping.connector = it->get<int>();
    }
}

// ---------------------------------------------------------------------------
// Reading a payload
// ---------------------------------------------------------------------------

std::optional<Envelope> read_envelope(std::string_view topic, const nlohmann::json& payload) {
    const auto address = parse_topic(topic);
    if (not address.has_value() or not payload.is_object()) {
        return std::nullopt;
    }

    Envelope envelope;
    envelope.module_id = address->module_id;
    envelope.set = address->set;

    if (const auto it = payload.find("version"); it != payload.end()) {
        if (not it->is_number_integer() or it->get<int>() != ENVELOPE_VERSION) {
            return std::nullopt;
        }
        envelope.version = it->get<int>();
    }

    const auto values = payload.find("values");
    if (values == payload.end() or not values->is_object()) {
        return std::nullopt;
    }
    for (const auto& [name, value] : values->items()) {
        envelope.values[name] = value.get<Value>();
    }

    if (const auto it = payload.find("module_type"); it != payload.end() and it->is_string()) {
        envelope.module_type = it->get<std::string>();
    }
    if (const auto it = payload.find("instance"); it != payload.end() and it->is_string()) {
        envelope.instance = it->get<std::string>();
    }
    if (const auto it = payload.find("timestamp"); it != payload.end() and it->is_string()) {
        envelope.timestamp = it->get<std::string>();
    }
    if (const auto it = payload.find("mapping");
        it != payload.end() and it->is_object() and it->contains("evse")) {
        envelope.mapping = it->get<EvseMapping>();
    }
    if (const auto it = payload.find("seq"); it != payload.end() and it->is_number_unsigned()) {
        envelope.seq = it->get<std::uint64_t>();
    }
    if (const auto it = payload.find("labels"); it != payload.end() and it->is_object()) {
        for (const auto& [key, label] : it->items()) {
            if (label.is_string()) {
                envelope.labels[key] = label.get<std::string>();
            }
        }
    }

    return envelope;
}

nlohmann::json to_payload(const Envelope& envelope) {
    auto payload = nlohmann::json::object();
    payload["version"] = envelope.version;
    payload["module_type"] = envelope.module_type;
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
