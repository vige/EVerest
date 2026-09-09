// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "telemetryImpl.hpp"

namespace module {
namespace diagnostics {

using types::telemetry::EntryType;

void telemetryImpl::init() {
    types::telemetry::SetDefinition definition;
    definition.module_type = "TelemetryProducerExample";
    definition.set = "diagnostics";
    definition.description = "Slow changing device diagnostics";
    definition.max_publish_rate_hz = 0.2f;
    definition.entries = {
        entry_of("uptime_s", "Seconds since the module started", EntryType::integer, "s"),
        entry_of("error_count", "Errors since the module started", EntryType::integer),
        entry_of("serial", "Device serial number", EntryType::string),
    };

    publisher = std::make_unique<SetPublisher>(
        std::move(definition), mod->info.id,
        [this](const types::telemetry::Update& update) { this->publish_update(update); });

    if (const auto mapping = get_mapping(); mapping.has_value()) {
        types::telemetry::Mapping telemetry_mapping;
        telemetry_mapping.evse = mapping->evse;
        telemetry_mapping.connector = mapping->connector;
        publisher->set_mapping(telemetry_mapping);
    }

    mod->diagnostics_publisher = publisher.get();
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
    EVLOG_info << "telemetry diagnostics: " << module_id << " wants " << wanted.size() << " entry(ies)";
    publisher->set_interest(module_id, wanted);
}

} // namespace diagnostics
} // namespace module
