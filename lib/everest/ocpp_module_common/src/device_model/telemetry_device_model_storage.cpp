// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/ocpp_module_common/device_model/telemetry_device_model_storage.hpp>

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>

#include <fmt/format.h>

#include <everest/logging.hpp>
#include <ocpp/v2/comparators.hpp>

namespace ocpp_module_common::device_model {


std::string render_value(const otlp::Sample& sample, ocpp::v2::DataEnum type) {
    // an if chain rather than a switch: this library is built with -Werror=switch-enum, and the
    // remaining data types are all rendered the same way
    if (type == ocpp::v2::DataEnum::boolean) {
        // OTLP has no boolean metric: a producer says false with 0 and true with anything else
        return sample.value() != 0.0 ? "true" : "false";
    }
    if (type == ocpp::v2::DataEnum::integer) {
        return std::to_string(sample.is_integer ? sample.as_int
                                                : static_cast<std::int64_t>(std::llround(sample.as_double)));
    }
    if (sample.is_integer) {
        return std::to_string(sample.as_int);
    }
    // The default ostream precision drops trailing zeroes, which is what keeps 40.5 from reaching
    // the CSMS as 40.500000.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << sample.as_double;
    return out.str();
}

TelemetryDeviceModelStorage::TelemetryDeviceModelStorage(const std::vector<TelemetryMapping>& mappings) {
    for (const auto& mapping : mappings) {
        // The whole device model is known here, before anything has been measured: the mapping
        // file describes every variable, so a CSMS reading a base report at boot sees them all,
        // with no value rather than with a value the station never took.
        ocpp::v2::VariableMetaData meta;
        meta.characteristics = mapping.characteristics;
        meta.source = VARIABLE_SOURCE_TELEMETRY;
        const ocpp::v2::ComponentVariable target{mapping.component, mapping.variable, std::nullopt};
        this->model[mapping.component].insert_or_assign(mapping.variable, meta);
        this->table.insert_or_assign(target, mapping);
    }
}

std::size_t TelemetryDeviceModelStorage::on_export(const otlp::Export& exported) {
    std::size_t changed = 0;
    std::lock_guard<std::mutex> lock{this->mutex};
    for (const auto& sample : exported.samples) {
        for (const auto& [target, mapping] : this->table) {
            if (not mapping.selector.matches(sample)) {
                continue;
            }
            // Rendered as the data type the mapping declares, so the value and the characteristics
            // a CSMS was told cannot disagree.
            this->values[target] = render_value(sample, mapping.characteristics.dataType);
            ++changed;
        }
    }
    return changed;
}

const std::map<ocpp::v2::ComponentVariable, TelemetryMapping>& TelemetryDeviceModelStorage::mappings() const {
    return this->table;
}

ocpp::v2::DeviceModelMap TelemetryDeviceModelStorage::get_device_model() {
    if (this->monitor_store == nullptr) {
        return this->model;
    }

    // The composed storage keeps a variable only from the source that owns it, so this is the only
    // copy of these variables that survives the merge -- and it has to carry the monitors the
    // database holds for them, or a monitor set before a reboot would be persisted and then never
    // loaded. The structure stays ours; only the monitors come from the store.
    auto model_with_monitors = this->model;
    for (auto& [component, seeded] : this->monitor_store->get_device_model()) {
        const auto ours = model_with_monitors.find(component);
        if (ours == model_with_monitors.end()) {
            continue;
        }
        for (auto& [variable, meta] : seeded) {
            const auto variable_it = ours->second.find(variable);
            if (variable_it != ours->second.end()) {
                variable_it->second.monitors = std::move(meta.monitors);
            }
        }
    }
    return model_with_monitors;
}

std::optional<std::string> TelemetryDeviceModelStorage::read(const ocpp::v2::ComponentVariable& target) const {
    std::lock_guard<std::mutex> lock{this->mutex};
    const auto value = this->values.find(target);
    return value == this->values.end() ? std::nullopt : std::optional<std::string>{value->second};
}

std::optional<ocpp::v2::VariableAttribute>
TelemetryDeviceModelStorage::get_variable_attribute(const ocpp::v2::Component& component_id,
                                                    const ocpp::v2::Variable& variable_id,
                                                    const ocpp::v2::AttributeEnum& attribute_enum) {
    if (attribute_enum != ocpp::v2::AttributeEnum::Actual) {
        // A measurement has no target, minimum or maximum setting; only what it currently reads.
        return std::nullopt;
    }
    const ocpp::v2::ComponentVariable target{component_id, variable_id, std::nullopt};
    if (this->table.find(target) == this->table.end()) {
        return std::nullopt;
    }

    ocpp::v2::VariableAttribute attribute;
    attribute.type = ocpp::v2::AttributeEnum::Actual;
    attribute.mutability = ocpp::v2::MutabilityEnum::ReadOnly;
    attribute.persistent = false;
    attribute.constant = false;
    const auto value = read(target);
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

void TelemetryDeviceModelStorage::set_monitor_store(std::shared_ptr<ocpp::v2::DeviceModelStorageInterface> store) {
    this->monitor_store = std::move(store);
}

std::map<ocpp::v2::ComponentKey, std::vector<ocpp::v2::DeviceModelVariable>>
TelemetryDeviceModelStorage::component_config() const {
    std::map<ocpp::v2::ComponentKey, std::vector<ocpp::v2::DeviceModelVariable>> config;
    for (const auto& [target, mapping] : this->table) {
        ocpp::v2::ComponentKey component;
        component.name = mapping.component.name.get();
        if (mapping.component.instance.has_value()) {
            component.instance = mapping.component.instance->get();
        }
        if (mapping.component.evse.has_value()) {
            component.evse_id = mapping.component.evse->id;
            component.connector_id = mapping.component.evse->connectorId;
        }

        ocpp::v2::DeviceModelVariable variable;
        variable.name = mapping.variable.name.get();
        if (mapping.variable.instance.has_value()) {
            variable.instance = mapping.variable.instance->get();
        }
        variable.characteristics = this->model.at(mapping.component).at(mapping.variable).characteristics;
        // The source is what routes reads back to this storage: the composed storage reads it out of
        // the row the seeding writes.
        variable.source = VARIABLE_SOURCE_TELEMETRY;

        // One Actual attribute, so the variable is addressable and can carry a monitor. Its value is
        // never written here -- a read is answered from memory -- so it is neither persistent nor
        // seeded with a default that the station never measured.
        ocpp::v2::DbVariableAttribute attribute;
        attribute.variable_attribute.type = ocpp::v2::AttributeEnum::Actual;
        attribute.variable_attribute.mutability = ocpp::v2::MutabilityEnum::ReadOnly;
        attribute.variable_attribute.persistent = false;
        attribute.variable_attribute.constant = false;
        variable.attributes.push_back(attribute);

        config[component].push_back(std::move(variable));
    }
    return config;
}

std::optional<ocpp::v2::VariableMonitoringMeta>
TelemetryDeviceModelStorage::set_monitoring_data(const ocpp::v2::SetMonitoringData& data,
                                                 const ocpp::v2::VariableMonitorType type) {
    if (this->monitor_store == nullptr) {
        return std::nullopt;
    }
    return this->monitor_store->set_monitoring_data(data, type);
}

bool TelemetryDeviceModelStorage::update_monitoring_reference(const int32_t monitor_id,
                                                              const std::string& reference_value) {
    return this->monitor_store == nullptr ? false
                                          : this->monitor_store->update_monitoring_reference(monitor_id,
                                                                                              reference_value);
}

std::vector<ocpp::v2::VariableMonitoringMeta>
TelemetryDeviceModelStorage::get_monitoring_data(const std::vector<ocpp::v2::MonitoringCriterionEnum>& criteria,
                                                 const ocpp::v2::Component& component_id,
                                                 const ocpp::v2::Variable& variable_id) {
    if (this->monitor_store == nullptr) {
        return {};
    }
    return this->monitor_store->get_monitoring_data(criteria, component_id, variable_id);
}

ocpp::v2::ClearMonitoringStatusEnum TelemetryDeviceModelStorage::clear_variable_monitor(int monitor_id,
                                                                                        bool allow_protected) {
    if (this->monitor_store == nullptr) {
        return ocpp::v2::ClearMonitoringStatusEnum::NotFound;
    }
    return this->monitor_store->clear_variable_monitor(monitor_id, allow_protected);
}

int32_t TelemetryDeviceModelStorage::clear_custom_variable_monitors() {
    // The store is shared with the OCPP source, which is asked for its own custom monitors in the
    // same fan-out. Answering here as well would clear them once and count them twice.
    return 0;
}

void TelemetryDeviceModelStorage::check_integrity() {
}

std::shared_ptr<ocpp::v2::DeviceModelStorageSqlite>
make_ocpp_device_model_storage(const std::filesystem::path& database_path,
                               const std::filesystem::path& migration_path,
                               const std::filesystem::path& config_path,
                               const std::shared_ptr<TelemetryDeviceModelStorage>& telemetry) {
    if (telemetry == nullptr) {
        return std::make_shared<ocpp::v2::DeviceModelStorageSqlite>(database_path, migration_path, config_path);
    }

    auto configs = ocpp::v2::get_all_component_configs(config_path);
    for (const auto& [component, variables] : telemetry->component_config()) {
        auto& into = configs[component];
        into.insert(into.end(), variables.begin(), variables.end());
    }

    ocpp::v2::InitDeviceModelDb init(database_path, migration_path);
    init.initialize_database(configs, false);

    auto storage = std::make_shared<ocpp::v2::DeviceModelStorageSqlite>(database_path);
    telemetry->set_monitor_store(storage);
    return storage;
}

} // namespace ocpp_module_common::device_model
