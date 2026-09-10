// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/ocpp_module_common/device_model/telemetry_device_model_storage.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <fmt/format.h>

#include <everest/logging.hpp>
#include <ocpp/v2/comparators.hpp>

namespace ocpp_module_common::device_model {

namespace {

/// \returns the OCPP data type of the telemetry entry type \p type
///
/// A string entry with an allowed-value list becomes an OptionList rather than a string, because
/// that is what lets a CSMS render it as a choice instead of free text. The mapping cannot override
/// this: the type is the publisher's to state.
ocpp::v2::DataEnum data_type_of(const types::telemetry::EntryDefinition& entry) {
    switch (entry.type) {
    case types::telemetry::EntryType::boolean:
        return ocpp::v2::DataEnum::boolean;
    case types::telemetry::EntryType::integer:
        return ocpp::v2::DataEnum::integer;
    case types::telemetry::EntryType::number:
        return ocpp::v2::DataEnum::decimal;
    case types::telemetry::EntryType::string:
        break;
    }
    return entry.values_list.has_value() and not entry.values_list->empty() ? ocpp::v2::DataEnum::OptionList
                                                                            : ocpp::v2::DataEnum::string;
}

/// \returns the characteristics of \p entry, as the CSMS sees them
ocpp::v2::VariableCharacteristics characteristics_of(const types::telemetry::EntryDefinition& entry) {
    ocpp::v2::VariableCharacteristics characteristics;
    characteristics.dataType = data_type_of(entry);
    // Every telemetry variable is a candidate for a monitor: a stream of measurements is exactly what
    // a CSMS wants a threshold or a delta on.
    characteristics.supportsMonitoring = true;
    if (entry.unit.has_value() and not entry.unit->empty()) {
        characteristics.unit = entry.unit->substr(0, 16);
    }
    if (entry.minimum.has_value()) {
        characteristics.minLimit = static_cast<float>(*entry.minimum);
    }
    if (entry.maximum.has_value()) {
        characteristics.maxLimit = static_cast<float>(*entry.maximum);
    }
    if (entry.values_list.has_value() and not entry.values_list->empty()) {
        std::string list;
        for (const auto& value : *entry.values_list) {
            list += list.empty() ? value : "," + value;
        }
        characteristics.valuesList = list.substr(0, 1000);
    }
    return characteristics;
}

/// \returns the entry \p name of \p definition, or nullptr when it does not declare one
const types::telemetry::EntryDefinition* entry_of(const types::telemetry::SetDefinition& definition,
                                                  const std::string& name) {
    const auto it = std::find_if(definition.entries.begin(), definition.entries.end(),
                                 [&name](const auto& entry) { return entry.name == name; });
    return it == definition.entries.end() ? nullptr : &*it;
}

/// \returns true when \p type is one of \p criteria, or when no criteria were given
bool matches_criteria(ocpp::v2::MonitorEnum type, const std::vector<ocpp::v2::MonitoringCriterionEnum>& criteria) {
    if (criteria.empty()) {
        return true;
    }
    for (const auto& criterion : criteria) {
        switch (criterion) {
        case ocpp::v2::MonitoringCriterionEnum::ThresholdMonitoring:
            if (type == ocpp::v2::MonitorEnum::UpperThreshold or type == ocpp::v2::MonitorEnum::LowerThreshold) {
                return true;
            }
            break;
        case ocpp::v2::MonitoringCriterionEnum::DeltaMonitoring:
            if (type == ocpp::v2::MonitorEnum::Delta) {
                return true;
            }
            break;
        case ocpp::v2::MonitoringCriterionEnum::PeriodicMonitoring:
            if (type == ocpp::v2::MonitorEnum::Periodic or type == ocpp::v2::MonitorEnum::PeriodicClockAligned) {
                return true;
            }
            break;
        }
    }
    return false;
}

} // namespace

std::string render_value(const nlohmann::json& value, types::telemetry::EntryType type) {
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
        const auto number = value.get<double>();
        if (type == types::telemetry::EntryType::integer) {
            return std::to_string(static_cast<std::int64_t>(std::llround(number)));
        }
        // The default ostream precision drops trailing zeroes, which is what keeps 40.5 from
        // reaching the CSMS as 40.500000.
        std::ostringstream out;
        out << number;
        return out.str();
    }
    // Anything else is a producer that published a non-scalar into a scalar entry. Report it
    // verbatim rather than dropping it, so the defect is visible at the CSMS instead of silent.
    return value.dump();
}

TelemetryDeviceModelStorage::TelemetryDeviceModelStorage(
    const std::vector<TelemetryMapping>& mappings,
    const std::map<Everest::telemetry::SetKey, types::telemetry::SetDefinition>& definitions) {
    for (const auto& mapping : mappings) {
        const auto definition = definitions.find(mapping.flow);
        if (definition == definitions.end()) {
            // Not necessarily a defect in the mapping file: one file can describe a family of
            // stations, and this one may simply not be wired to that producer.
            this->dropped.push_back(
                fmt::format("{}: no telemetry set '{}' is wired to this module from module '{}'",
                            mapping.to_string(), mapping.flow.set, mapping.flow.module_id));
            continue;
        }
        const auto* entry = entry_of(definition->second, mapping.entry);
        if (entry == nullptr) {
            this->dropped.push_back(
                fmt::format("{}: the set does not declare an entry '{}'", mapping.to_string(), mapping.entry));
            continue;
        }

        ocpp::v2::VariableMetaData meta;
        meta.characteristics = characteristics_of(*entry);
        meta.source = VARIABLE_SOURCE_TELEMETRY;
        const ocpp::v2::ComponentVariable target{mapping.component, mapping.variable, std::nullopt};
        this->model[mapping.component].insert_or_assign(mapping.variable, meta);
        this->table.insert_or_assign(target, mapping);
        this->entry_types.insert_or_assign(target, entry->type);
    }
}

std::size_t TelemetryDeviceModelStorage::on_update(const types::telemetry::Update& update) {
    const Everest::telemetry::SetKey key{update.module_id, update.set};
    std::size_t changed = 0;
    std::lock_guard<std::mutex> lock{this->mutex};
    for (const auto& [target, mapping] : this->table) {
        if (not(mapping.flow == key)) {
            continue;
        }
        const auto value = update.values.find(mapping.entry);
        if (value == update.values.end()) {
            continue;
        }
        // Rendered with the type the publisher declared, which is also what the characteristics were
        // built from, so the value and its dataType cannot disagree.
        this->values[key][mapping.entry] = render_value(value->second, this->entry_types.at(target));
        ++changed;
    }
    return changed;
}

const std::map<ocpp::v2::ComponentVariable, TelemetryMapping>& TelemetryDeviceModelStorage::mappings() const {
    return this->table;
}

const std::vector<std::string>& TelemetryDeviceModelStorage::unavailable() const {
    return this->dropped;
}

ocpp::v2::DeviceModelMap TelemetryDeviceModelStorage::get_device_model() {
    return this->model;
}

std::optional<std::string> TelemetryDeviceModelStorage::read(const TelemetryMapping& mapping) const {
    std::lock_guard<std::mutex> lock{this->mutex};
    const auto flow = this->values.find(mapping.flow);
    if (flow == this->values.end()) {
        return std::nullopt;
    }
    const auto value = flow->second.find(mapping.entry);
    return value == flow->second.end() ? std::nullopt : std::optional<std::string>{value->second};
}

std::optional<ocpp::v2::VariableAttribute>
TelemetryDeviceModelStorage::get_variable_attribute(const ocpp::v2::Component& component_id,
                                                    const ocpp::v2::Variable& variable_id,
                                                    const ocpp::v2::AttributeEnum& attribute_enum) {
    if (attribute_enum != ocpp::v2::AttributeEnum::Actual) {
        // A measurement has no target, minimum or maximum setting; only what it currently reads.
        return std::nullopt;
    }
    const auto mapping = this->table.find(ocpp::v2::ComponentVariable{component_id, variable_id, std::nullopt});
    if (mapping == this->table.end()) {
        return std::nullopt;
    }

    ocpp::v2::VariableAttribute attribute;
    attribute.type = ocpp::v2::AttributeEnum::Actual;
    attribute.mutability = ocpp::v2::MutabilityEnum::ReadOnly;
    attribute.persistent = false;
    attribute.constant = false;
    const auto value = read(mapping->second);
    if (value.has_value()) {
        attribute.value = value->substr(0, 2500);
    }
    return attribute;
}

std::vector<ocpp::v2::VariableAttribute>
TelemetryDeviceModelStorage::get_variable_attributes(const ocpp::v2::Component& component_id,
                                                     const ocpp::v2::Variable& variable_id,
                                                     const std::optional<ocpp::v2::AttributeEnum>& attribute_enum) {
    if (attribute_enum.has_value() and *attribute_enum != ocpp::v2::AttributeEnum::Actual) {
        return {};
    }
    const auto attribute = get_variable_attribute(component_id, variable_id, ocpp::v2::AttributeEnum::Actual);
    if (not attribute.has_value()) {
        return {};
    }
    return {*attribute};
}

ocpp::v2::SetVariableStatusEnum TelemetryDeviceModelStorage::set_variable_attribute_value(
    const ocpp::v2::Component& component_id, const ocpp::v2::Variable& variable_id,
    const ocpp::v2::AttributeEnum& attribute_enum, const std::string& value, const std::string& source) {
    // Telemetry flows one way. A CSMS that could write a measurement back could make the station
    // report a value it never took.
    return ocpp::v2::SetVariableStatusEnum::Rejected;
}

std::vector<ocpp::v2::VariableMonitoringMeta>
TelemetryDeviceModelStorage::monitors_of(const ocpp::v2::ComponentVariable& target,
                                         const std::vector<ocpp::v2::MonitoringCriterionEnum>& criteria) const {
    std::vector<ocpp::v2::VariableMonitoringMeta> found;
    const auto it = this->monitors.find(target);
    if (it == this->monitors.end()) {
        return found;
    }
    for (const auto& [id, monitor] : it->second) {
        if (not matches_criteria(monitor.monitor.type, criteria)) {
            continue;
        }
        found.push_back(monitor);
    }
    return found;
}

std::optional<ocpp::v2::VariableMonitoringMeta>
TelemetryDeviceModelStorage::set_monitoring_data(const ocpp::v2::SetMonitoringData& data,
                                                 const ocpp::v2::VariableMonitorType type) {
    const ocpp::v2::ComponentVariable target{data.component, data.variable, std::nullopt};
    const auto mapping = this->table.find(target);
    if (mapping == this->table.end()) {
        return std::nullopt;
    }

    ocpp::v2::VariableMonitoringMeta meta;
    meta.type = type;
    meta.monitor.type = data.type;
    meta.monitor.severity = data.severity;
    meta.monitor.value = data.value;
    meta.monitor.transaction = data.transaction.value_or(false);

    if (data.type == ocpp::v2::MonitorEnum::Delta) {
        // A delta is measured from a reference, so a delta monitor on an entry that has not arrived
        // yet has nothing to measure from. Refusing is better than seeding a zero, which would fire
        // once, spuriously, the moment the first real value shows up.
        const auto value = read(mapping->second);
        if (not value.has_value()) {
            EVLOG_warning << "telemetry: refusing a delta monitor on " << mapping->second.to_string()
                          << ": no value has been published yet, so there is no reference to measure from";
            return std::nullopt;
        }
        meta.reference_value = *value;
    }

    std::lock_guard<std::mutex> lock{this->monitor_mutex};
    if (data.id.has_value()) {
        // An update names the id. Replacing in place keeps the id the CSMS holds valid.
        meta.monitor.id = *data.id;
    } else {
        meta.monitor.id = this->next_monitor_id++;
    }
    this->monitors[target][meta.monitor.id] = meta;
    return meta;
}

bool TelemetryDeviceModelStorage::update_monitoring_reference(const int32_t monitor_id,
                                                              const std::string& reference_value) {
    std::lock_guard<std::mutex> lock{this->monitor_mutex};
    for (auto& [target, by_id] : this->monitors) {
        const auto it = by_id.find(monitor_id);
        if (it != by_id.end()) {
            it->second.reference_value = reference_value;
            return true;
        }
    }
    return false;
}

std::vector<ocpp::v2::VariableMonitoringMeta>
TelemetryDeviceModelStorage::get_monitoring_data(const std::vector<ocpp::v2::MonitoringCriterionEnum>& criteria,
                                                 const ocpp::v2::Component& component_id,
                                                 const ocpp::v2::Variable& variable_id) {
    std::lock_guard<std::mutex> lock{this->monitor_mutex};
    return monitors_of(ocpp::v2::ComponentVariable{component_id, variable_id, std::nullopt}, criteria);
}

ocpp::v2::ClearMonitoringStatusEnum TelemetryDeviceModelStorage::clear_variable_monitor(int monitor_id,
                                                                                        bool allow_protected) {
    std::lock_guard<std::mutex> lock{this->monitor_mutex};
    for (auto& [target, by_id] : this->monitors) {
        const auto it = by_id.find(monitor_id);
        if (it == by_id.end()) {
            continue;
        }
        if (not allow_protected and it->second.type != ocpp::v2::VariableMonitorType::CustomMonitor) {
            return ocpp::v2::ClearMonitoringStatusEnum::Rejected;
        }
        by_id.erase(it);
        return ocpp::v2::ClearMonitoringStatusEnum::Accepted;
    }
    // Not ours. The composed storage asks every source in turn, because a monitor id carries no
    // hint of which one issued it.
    return ocpp::v2::ClearMonitoringStatusEnum::NotFound;
}

int32_t TelemetryDeviceModelStorage::clear_custom_variable_monitors() {
    std::lock_guard<std::mutex> lock{this->monitor_mutex};
    std::int32_t cleared = 0;
    for (auto& [target, by_id] : this->monitors) {
        for (auto it = by_id.begin(); it != by_id.end();) {
            if (it->second.type == ocpp::v2::VariableMonitorType::CustomMonitor) {
                it = by_id.erase(it);
                ++cleared;
            } else {
                ++it;
            }
        }
    }
    return cleared;
}

void TelemetryDeviceModelStorage::check_integrity() {
}

} // namespace ocpp_module_common::device_model
