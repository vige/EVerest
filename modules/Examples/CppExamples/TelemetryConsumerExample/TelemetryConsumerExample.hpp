// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef TELEMETRY_CONSUMER_EXAMPLE_HPP
#define TELEMETRY_CONSUMER_EXAMPLE_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 3
//

#include "ld-ev.hpp"


// headers for required interface implementations
#include <generated/interfaces/telemetry/Interface.hpp>

// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1
// insert your custom include headers here
#include <memory>
#include <string>
#include <vector>

#include <everest/telemetry/sink.hpp>
// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1

namespace module {



struct Conf {
    std::string filter_module_id;
    std::string filter_module_type;
    std::string filter_set;
    std::string filter_entries;
    bool print_definitions;


};

class TelemetryConsumerExample : public Everest::ModuleBase {
public:
    TelemetryConsumerExample() = delete;
    TelemetryConsumerExample(
        const ModuleInfo& info,
        std::vector<std::unique_ptr<telemetryIntf>> r_telemetry,
        Conf& config
    ) :
        ModuleBase(info),
        r_telemetry(std::move(r_telemetry)),
        config(config)
    {};

    const std::vector<std::unique_ptr<telemetryIntf>> r_telemetry;
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

    /// \brief Convenience over r_telemetry: it holds the definitions, translates the configured
    /// filter into per-slot set_interest calls and prunes what this sink did not ask for.
    std::unique_ptr<Everest::telemetry::Sink> sink;

    /// \returns the filter the module config describes
    Everest::telemetry::Filter configured_filter() const;

    void log_definitions() const;
    void log_update(const types::telemetry::Update& update) const;
    // ev@211cfdbe-f69a-4cd6-a4ec-f8aaa3d1b6c8:v1

};

// ev@087e516b-124c-48df-94fb-109508c7cda9:v1
// insert other definitions here
// ev@087e516b-124c-48df-94fb-109508c7cda9:v1

} // namespace module

#endif // TELEMETRY_CONSUMER_EXAMPLE_HPP
