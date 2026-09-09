// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef TELEMETRY_CONSUMER_EXAMPLE_HPP
#define TELEMETRY_CONSUMER_EXAMPLE_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 3
//

#include "ld-ev.hpp"

// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1
// insert your custom include headers here
#include <memory>
#include <string>

#include <utils/mqtt_abstraction.hpp>
#include <utils/telemetry/consumer.hpp>
// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1

namespace module {

struct Conf {
    std::string filter_module_id;
    std::string filter_set;
    bool print_payload;
};

class TelemetryConsumerExample : public Everest::ModuleBase {
public:
    TelemetryConsumerExample() = delete;
    TelemetryConsumerExample(const ModuleInfo& info, Everest::MqttProvider& mqtt_provider, Conf& config) :
        ModuleBase(info), mqtt(mqtt_provider), config(config) {
    }

    Everest::MqttProvider& mqtt;
    const Conf& config;

    // ev@1fce4c5e-0ab8-41bb-90f7-14277703d2ac:v1
    // insert your public definitions here
    // ev@1fce4c5e-0ab8-41bb-90f7-14277703d2ac:v1

protected:
    // ev@4714b2ab-a24f-4b95-ab81-36439e1478de:v1
    // insert your protected definitions here
    // ev@4714b2ab-a24f-4b95-ab81-36439e1478de:v1

private:
    friend class LdEverest;
    void init();
    void ready();
    void shutdown();

    // ev@211cfdbe-f69a-4cd6-a4ec-f8aaa3d1b6c8:v1
    // insert your private definitions here

    /// \brief The telemetry consumer needs an MQTTAbstraction, which the module API does not hand
    /// out, so this module opens its own connection to the same broker. Once the framework exposes
    /// a telemetry consumer to modules directly, this goes away.
    std::unique_ptr<Everest::MQTTAbstraction> telemetry_mqtt;
    std::unique_ptr<Everest::telemetry::Consumer> consumer;
    Everest::telemetry::Consumer::Subscription subscription;

    void print(const Everest::telemetry::Envelope& envelope) const;
    // ev@211cfdbe-f69a-4cd6-a4ec-f8aaa3d1b6c8:v1
};

// ev@087e516b-124c-48df-94fb-109508c7cda9:v1
// insert other definitions here
// ev@087e516b-124c-48df-94fb-109508c7cda9:v1

} // namespace module

#endif // TELEMETRY_CONSUMER_EXAMPLE_HPP
