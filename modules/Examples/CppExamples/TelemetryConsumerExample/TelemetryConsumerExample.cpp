// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "TelemetryConsumerExample.hpp"

#include <sstream>

namespace module {

namespace {

/// \returns the comma separated \p list split and trimmed, empty entries dropped
std::vector<std::string> split_entries(const std::string& list) {
    std::vector<std::string> entries;
    std::stringstream stream{list};
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        const auto begin = entry.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            continue;
        }
        entries.push_back(entry.substr(begin, entry.find_last_not_of(" \t") - begin + 1));
    }
    return entries;
}

} // namespace

Everest::telemetry::Filter TelemetryConsumerExample::configured_filter() const {
    Everest::telemetry::Filter filter;
    if (not config.filter_module_id.empty()) {
        filter.module_id = config.filter_module_id;
    }
    if (not config.filter_module_type.empty()) {
        filter.module_type = config.filter_module_type;
    }
    if (not config.filter_set.empty()) {
        filter.set = config.filter_set;
    }
    filter.entries = split_entries(config.filter_entries);
    return filter;
}

void TelemetryConsumerExample::init() {
    sink = std::make_unique<Everest::telemetry::Sink>(r_telemetry, info.id);

    // Subscriptions belong in init: the snapshot a publisher sends when interest changes is a
    // normal update, and it is gone if no handler is registered when it arrives.
    sink->subscribe([this](const types::telemetry::Update& update) { this->log_update(update); });

    EVLOG_info << "telemetry consumer: " << r_telemetry.size() << " set(s) wired";
}

void TelemetryConsumerExample::ready() {
    const auto resolved = sink->resolve_definitions();
    EVLOG_info << "telemetry consumer: " << resolved << " of " << r_telemetry.size()
               << " set(s) answered get_definition";
    if (config.print_definitions) {
        log_definitions();
    }

    const auto filter = configured_filter();
    const auto interested = sink->declare_interest(filter);
    EVLOG_info << "telemetry consumer: declared interest in " << interested << " set(s)";
    if (interested == 0) {
        EVLOG_warning << "telemetry consumer: nothing matched the filter, so nothing will be published";
    }
}

void TelemetryConsumerExample::log_definitions() const {
    for (const auto& [key, definition] : sink->definitions()) {
        std::stringstream entries;
        for (const auto& entry : definition.entries) {
            entries << " " << entry.name << ":" << types::telemetry::entry_type_to_string(entry.type);
            if (entry.unit.has_value()) {
                entries << "[" << *entry.unit << "]";
            }
        }
        EVLOG_info << "telemetry definition " << key.to_string() << " (" << definition.module_type << ")"
                   << (definition.max_publish_rate_hz.has_value()
                           ? " max " + std::to_string(*definition.max_publish_rate_hz) + " Hz"
                           : std::string{})
                   << " entries:" << entries.str();
    }
}

void TelemetryConsumerExample::log_update(const types::telemetry::Update& update) const {
    const Everest::telemetry::SetKey key{update.module_id, update.set};

    std::stringstream mapping;
    if (update.mapping.has_value()) {
        mapping << " evse=" << update.mapping->evse;
        if (update.mapping->connector.has_value()) {
            mapping << " connector=" << *update.mapping->connector;
        }
    }

    std::stringstream values;
    for (const auto& [entry, value] : update.values) {
        values << " " << entry << "=" << value.dump();
    }

    EVLOG_info << "telemetry update " << key.to_string() << " (" << update.module_type << ")" << mapping.str() << " at "
               << update.timestamp << ":" << values.str();

    // What the sink knows, as opposed to what this message carried: de-duplication means an entry
    // absent from an update is unchanged, not gone.
    EVLOG_debug << "telemetry state " << key.to_string() << ": " << json(sink->values(key)).dump();
}

void TelemetryConsumerExample::shutdown() {
    if (sink) {
        sink->withdraw();
    }
}

} // namespace module
