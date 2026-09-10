// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "telemetry_mapping.hpp"

#include <algorithm>

#include <fmt/format.h>
#include <ocpp/v2/comparators.hpp>
#include <nlohmann/json.hpp>

#include <everest/utils/yaml_loader.hpp>

namespace module::telemetry_dm {

namespace {

/// \returns the string at \p key of \p object, or nothing when it is absent or not a string
std::optional<std::string> string_at(const nlohmann::ordered_json& object, const std::string& key) {
    const auto it = object.find(key);
    if (it == object.end() or not it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

/// \returns the integer at \p key of \p object, or nothing when it is absent or not an integer
std::optional<std::int32_t> int_at(const nlohmann::ordered_json& object, const std::string& key) {
    const auto it = object.find(key);
    if (it == object.end() or not it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<std::int32_t>();
}

} // namespace

std::string TelemetryMapping::to_string() const {
    std::string target = component.name.get();
    if (component.instance.has_value()) {
        target += "(" + component.instance->get() + ")";
    }
    if (component.evse.has_value()) {
        target += fmt::format("[evse {}", component.evse->id);
        if (component.evse->connectorId.has_value()) {
            target += fmt::format(", connector {}", *component.evse->connectorId);
        }
        target += "]";
    }
    target += "/" + variable.name.get();
    if (variable.instance.has_value()) {
        target += "(" + variable.instance->get() + ")";
    }
    return fmt::format("{} <- {}.{}", target, flow.to_string(), entry);
}

TelemetryMappingLoad load_telemetry_mappings(const std::filesystem::path& path) {
    TelemetryMappingLoad load;

    nlohmann::ordered_json document;
    try {
        document = Everest::load_yaml(path);
    } catch (const std::exception& e) {
        load.errors.push_back(fmt::format("cannot read the telemetry mapping file {}: {}", path.string(), e.what()));
        return load;
    }

    const auto list = document.find("telemetry_mappings");
    if (list == document.end() or not list->is_array()) {
        load.errors.push_back(
            fmt::format("the telemetry mapping file {} has no 'telemetry_mappings' list", path.string()));
        return load;
    }

    std::size_t index = 0;
    for (const auto& item : *list) {
        const auto position = index++;
        const auto reject = [&load, position](const std::string& why) {
            load.rejected.push_back(fmt::format("telemetry_mappings[{}]: {}", position, why));
        };

        if (not item.is_object()) {
            reject("not an object");
            continue;
        }
        const auto ocpp = item.find("ocpp");
        const auto everest = item.find("everest");
        if (ocpp == item.end() or not ocpp->is_object()) {
            reject("no 'ocpp' object");
            continue;
        }
        if (everest == item.end() or not everest->is_object()) {
            reject("no 'everest' object");
            continue;
        }

        const auto component_json = ocpp->find("component");
        const auto variable_json = ocpp->find("variable");
        if (component_json == ocpp->end() or not component_json->is_object()) {
            reject("no 'ocpp.component' object");
            continue;
        }
        if (variable_json == ocpp->end() or not variable_json->is_object()) {
            reject("no 'ocpp.variable' object");
            continue;
        }

        const auto component_name = string_at(*component_json, "name");
        const auto variable_name = string_at(*variable_json, "name");
        const auto module_id = string_at(*everest, "module_id");
        const auto set = string_at(*everest, "telemetry_set");
        const auto entry = string_at(*everest, "entry");
        if (not component_name.has_value() or component_name->empty()) {
            reject("no 'ocpp.component.name'");
            continue;
        }
        if (not variable_name.has_value() or variable_name->empty()) {
            reject("no 'ocpp.variable.name'");
            continue;
        }
        if (not module_id.has_value() or module_id->empty()) {
            reject("no 'everest.module_id'");
            continue;
        }
        if (not set.has_value() or set->empty()) {
            reject("no 'everest.telemetry_set'");
            continue;
        }
        if (not entry.has_value() or entry->empty()) {
            reject("no 'everest.entry'");
            continue;
        }

        TelemetryMapping mapping;
        mapping.component.name = *component_name;
        mapping.component.instance = string_at(*component_json, "instance");
        const auto evse_json = component_json->find("evse");
        if (evse_json != component_json->end()) {
            if (not evse_json->is_object()) {
                reject("'ocpp.component.evse' is not an object");
                continue;
            }
            const auto evse_id = int_at(*evse_json, "id");
            if (not evse_id.has_value()) {
                reject("no integer 'ocpp.component.evse.id'");
                continue;
            }
            ocpp::v2::EVSE evse;
            evse.id = *evse_id;
            evse.connectorId = int_at(*evse_json, "connector");
            mapping.component.evse = evse;
        }
        mapping.variable.name = *variable_name;
        mapping.variable.instance = string_at(*variable_json, "instance");
        mapping.flow = Everest::telemetry::SetKey{*module_id, *set};
        mapping.entry = *entry;

        // A device model variable is addressed by (component, variable) and nothing else, so two
        // mappings onto the same target would make the value served depend on map ordering.
        const auto clash = std::find_if(load.mappings.begin(), load.mappings.end(), [&mapping](const auto& other) {
            return other.component == mapping.component and other.variable == mapping.variable;
        });
        if (clash != load.mappings.end()) {
            reject(fmt::format("target already mapped by {}", clash->to_string()));
            continue;
        }

        load.mappings.push_back(std::move(mapping));
    }

    return load;
}

} // namespace module::telemetry_dm
