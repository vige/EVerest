// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "telemetryImpl.hpp"

namespace module {
namespace livedata {

using types::telemetry::EntryType;

void telemetryImpl::init() {
    types::telemetry::SetDefinition definition;
    definition.module_type = "TelemetryProducerExample";
    definition.set = "livedata";
    definition.description = "Live electrical measurements";
    definition.max_publish_rate_hz = 4;
    definition.entries = {
        entry_of("temperature_C", "Board temperature", EntryType::number, "Celsius", -40.0f, 120.0f),
        entry_of("frequency_Hz", "Grid frequency", EntryType::number, "Hz", 45.0f, 55.0f),
        entry_of("current_A", "Output current", EntryType::number, "A", 0.0f, 500.0f),
        entry_of("fw_state", "Firmware state", EntryType::string),
    };
    definition.entries.back().values_list = std::vector<std::string>{"Idle", "Measuring", "Error"};

    publisher = std::make_unique<SetPublisher>(
        std::move(definition), mod->info.id,
        [this](const types::telemetry::Update& update) { this->publish_update(update); });

    if (const auto mapping = get_mapping(); mapping.has_value()) {
        types::telemetry::Mapping telemetry_mapping;
        telemetry_mapping.evse = mapping->evse;
        telemetry_mapping.connector = mapping->connector;
        publisher->set_mapping(telemetry_mapping);
    }

    mod->livedata_publisher = publisher.get();
}

void telemetryImpl::ready() {
}

void telemetryImpl::shutdown() {
}

types::telemetry::SetDefinition telemetryImpl::handle_get_definition() {
    return publisher->definition();
}

void telemetryImpl::handle_set_interest(std::string& module_id, Array& entries) {
    std::vector<std::string> wanted;
    for (const auto& entry : entries) {
        wanted.push_back(entry.get<std::string>());
    }
    EVLOG_info << "telemetry livedata: " << module_id << " wants " << wanted.size() << " entry(ies)";
    publisher->set_interest(module_id, wanted);
}

} // namespace livedata
} // namespace module
