// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/ocpp_module_common/device_model/telemetry_mapping.hpp>

#include <algorithm>
#include <locale>
#include <sstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <ocpp/v2/comparators.hpp>

#include <everest/utils/yaml_loader.hpp>

namespace ocpp_module_common::device_model {

namespace {

/// The OCPP field lengths that would otherwise be discovered by a CiString throwing at start-up.
constexpr std::size_t UNIT_MAX = 16;
constexpr std::size_t VALUES_LIST_MAX = 1000;

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

/// \returns the number at \p key of \p object, or nothing when it is absent or not a number
std::optional<double> number_at(const nlohmann::ordered_json& object, const std::string& key) {
    const auto it = object.find(key);
    if (it == object.end() or not it->is_number()) {
        return std::nullopt;
    }
    return it->get<double>();
}

/// \brief A YAML scalar as the text an OTLP attribute would decode to.
///
/// A mapping is written by hand, so `evse: 1` is an integer to the YAML parser while the same
/// attribute on the wire decodes to "1". Rendering both the same way here is what lets the file be
/// written naturally instead of with everything quoted.
std::optional<std::string> scalar_as_text(const nlohmann::ordered_json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_float()) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(17);
        out << value.get<double>();
        return out.str();
    }
    return std::nullopt;
}

/// \returns the OCPP data type spelled \p name, or nothing when it is not one
std::optional<ocpp::v2::DataEnum> data_type_of(const std::string& name) {
    static const std::map<std::string, ocpp::v2::DataEnum> BY_NAME{
        {"string", ocpp::v2::DataEnum::string},         {"decimal", ocpp::v2::DataEnum::decimal},
        {"integer", ocpp::v2::DataEnum::integer},       {"dateTime", ocpp::v2::DataEnum::dateTime},
        {"boolean", ocpp::v2::DataEnum::boolean},       {"OptionList", ocpp::v2::DataEnum::OptionList},
        {"SequenceList", ocpp::v2::DataEnum::SequenceList}, {"MemberList", ocpp::v2::DataEnum::MemberList}};
    const auto found = BY_NAME.find(name);
    return found == BY_NAME.end() ? std::optional<ocpp::v2::DataEnum>{} : found->second;
}

} // namespace

bool MetricSelector::matches(const otlp::Sample& sample) const {
    if (sample.metric != this->metric) {
        return false;
    }
    for (const auto& [key, value] : this->attributes) {
        const auto found = sample.attributes.find(key);
        if (found == sample.attributes.end() or found->second != value) {
            return false;
        }
    }
    return true;
}

std::string MetricSelector::to_string() const {
    if (this->attributes.empty()) {
        return this->metric;
    }
    std::string rendered = this->metric + "{";
    bool first = true;
    for (const auto& [key, value] : this->attributes) {
        rendered += (first ? "" : ",") + key + "=" + value;
        first = false;
    }
    return rendered + "}";
}

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
    return fmt::format("{} <- {}", target, selector.to_string());
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
        const auto otel = item.find("otel");
        if (ocpp == item.end() or not ocpp->is_object()) {
            reject("no 'ocpp' object");
            continue;
        }
        if (otel == item.end() or not otel->is_object()) {
            reject("no 'otel' object");
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
        const auto metric = string_at(*otel, "metric");
        if (not component_name.has_value() or component_name->empty()) {
            reject("no 'ocpp.component.name'");
            continue;
        }
        if (not variable_name.has_value() or variable_name->empty()) {
            reject("no 'ocpp.variable.name'");
            continue;
        }
        if (not metric.has_value() or metric->empty()) {
            reject("no 'otel.metric'");
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

        mapping.selector.metric = *metric;
        const auto attributes_json = otel->find("attributes");
        if (attributes_json != otel->end()) {
            if (not attributes_json->is_object()) {
                reject("'otel.attributes' is not an object");
                continue;
            }
            bool attributes_ok = true;
            for (const auto& [key, value] : attributes_json->items()) {
                const auto text = scalar_as_text(value);
                if (not text.has_value()) {
                    reject(fmt::format("'otel.attributes.{}' is not a scalar", key));
                    attributes_ok = false;
                    break;
                }
                mapping.selector.attributes[key] = *text;
            }
            if (not attributes_ok) {
                continue;
            }
        }

        // Characteristics: the CSMS-facing description, which OTLP cannot carry.
        const auto characteristics_json = ocpp->find("characteristics");
        if (characteristics_json == ocpp->end() or not characteristics_json->is_object()) {
            reject("no 'ocpp.characteristics' object");
            continue;
        }
        const auto data_type_name = string_at(*characteristics_json, "dataType");
        if (not data_type_name.has_value()) {
            reject("no 'ocpp.characteristics.dataType'");
            continue;
        }
        const auto data_type = data_type_of(*data_type_name);
        if (not data_type.has_value()) {
            reject(fmt::format("'{}' is not an OCPP data type", *data_type_name));
            continue;
        }
        mapping.characteristics.dataType = *data_type;
        // Every telemetry variable is a candidate for a monitor: a stream of measurements is
        // exactly what a CSMS wants a threshold or a delta on.
        mapping.characteristics.supportsMonitoring = true;

        const auto unit = string_at(*characteristics_json, "unit");
        if (unit.has_value() and not unit->empty()) {
            mapping.characteristics.unit = unit->substr(0, UNIT_MAX);
        }
        const auto minimum = number_at(*characteristics_json, "min");
        if (minimum.has_value()) {
            mapping.characteristics.minLimit = static_cast<float>(*minimum);
        }
        const auto maximum = number_at(*characteristics_json, "max");
        if (maximum.has_value()) {
            mapping.characteristics.maxLimit = static_cast<float>(*maximum);
        }
        const auto values_json = characteristics_json->find("valuesList");
        if (values_json != characteristics_json->end()) {
            if (not values_json->is_array()) {
                reject("'ocpp.characteristics.valuesList' is not a list");
                continue;
            }
            std::string values;
            for (const auto& value : *values_json) {
                const auto text = scalar_as_text(value);
                if (not text.has_value()) {
                    continue;
                }
                values += values.empty() ? *text : "," + *text;
            }
            if (not values.empty()) {
                mapping.characteristics.valuesList = values.substr(0, VALUES_LIST_MAX);
            }
        }

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

} // namespace ocpp_module_common::device_model
