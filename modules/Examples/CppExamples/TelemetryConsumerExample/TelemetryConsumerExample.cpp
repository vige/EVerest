// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "TelemetryConsumerExample.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

#include <utils/config/mqtt_settings.hpp>

namespace module {

namespace {

/// \brief The broker the manager put this module on. The framework resolves it the same way, but
/// does not hand the settings to the module, so they are read again here.
Everest::MQTTSettings mqtt_settings_from_env() {
    const char* host = std::getenv("MQTT_SERVER_ADDRESS");
    const char* port = std::getenv("MQTT_SERVER_PORT");

    std::uint16_t broker_port = 1883;
    if (port != nullptr) {
        try {
            broker_port = static_cast<std::uint16_t>(std::stoul(port));
        } catch (const std::exception&) {
            EVLOG_warning << "MQTT_SERVER_PORT is not a number, falling back to 1883";
        }
    }

    return Everest::create_mqtt_settings(host != nullptr ? host : "127.0.0.1", broker_port, "everest/",
                                         "everest_external/");
}

std::string to_display_string(const Everest::telemetry::Value& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                return held ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return held;
            } else if constexpr (std::is_same_v<T, nlohmann::json>) {
                return held.dump();
            } else {
                return std::to_string(held);
            }
        },
        value);
}

} // namespace

void TelemetryConsumerExample::print(const Everest::telemetry::Envelope& envelope) const {
    std::cout << "\n=== telemetry " << envelope.address().to_string() << " ===\n"
              << "  module_type: " << envelope.module_type << "\n"
              << "  timestamp:   " << envelope.timestamp << "\n";

    if (envelope.mapping.has_value()) {
        std::cout << "  mapping:     evse " << envelope.mapping->evse;
        if (envelope.mapping->connector.has_value()) {
            std::cout << ", connector " << *envelope.mapping->connector;
        }
        std::cout << "\n";
    }
    if (envelope.seq.has_value()) {
        std::cout << "  seq:         " << *envelope.seq << "\n";
    }
    for (const auto& [key, label] : envelope.labels) {
        std::cout << "  label " << key << ": " << label << "\n";
    }

    for (const auto& [name, value] : envelope.values) {
        std::cout << "  " << name << " = " << to_display_string(value);

        const auto* definition = Everest::telemetry::find_entry(this->consumer->definitions(), envelope.module_id,
                                                                envelope.set, name);
        if (definition != nullptr) {
            std::cout << " [" << Everest::telemetry::to_string(definition->type);
            if (definition->unit.has_value()) {
                std::cout << " " << *definition->unit;
            }
            std::cout << "]";
        }
        std::cout << "\n";
    }

    if (this->config.print_payload) {
        std::cout << "  payload:     " << Everest::telemetry::to_payload(envelope).dump() << "\n";
    }
    std::cout << std::flush;
}

void TelemetryConsumerExample::init() {
    // The consumer library takes an MQTTAbstraction, which the module API does not expose, so this
    // module opens a second connection to the same broker.
    this->telemetry_mqtt = Everest::make_mqtt_abstraction(mqtt_settings_from_env());
    if (not this->telemetry_mqtt->connect()) {
        EVLOG_AND_THROW(Everest::EverestConfigError("TelemetryConsumerExample could not connect to the MQTT broker"));
    }
    this->telemetry_mqtt->spawn_main_loop_thread();

    this->consumer = std::make_unique<Everest::telemetry::Consumer>(*this->telemetry_mqtt);
}

void TelemetryConsumerExample::ready() {
    Everest::telemetry::Filter filter;
    if (not this->config.filter_module_id.empty()) {
        filter.module_id = this->config.filter_module_id;
    }
    if (not this->config.filter_set.empty()) {
        filter.set = this->config.filter_set;
    }

    this->subscription = this->consumer->subscribe(filter, [this](const Everest::telemetry::Envelope& envelope) {
        this->print(envelope);
    });

    std::cout << "TelemetryConsumerExample listening on " << this->subscription.topic() << std::endl;
}

void TelemetryConsumerExample::shutdown() {
    this->subscription.reset();
    this->consumer.reset();
    if (this->telemetry_mqtt) {
        this->telemetry_mqtt->stop_message_handling();
        this->telemetry_mqtt->disconnect();
    }
}

} // namespace module
