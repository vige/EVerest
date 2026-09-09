// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ModuleAdapterStub.hpp>
#include <everest/telemetry/sink.hpp>

/// \file
/// \brief A publisher behind a requirement slot, without a broker.
///
/// The tests drive the real generated telemetryIntf: the stub answers `get_definition`, records
/// `set_interest` and hands back the callback that `subscribe_update` registered, so what a test
/// exercises is the same code path a module takes at runtime, minus MQTT.
namespace Everest::telemetry::test {

/// \brief One publishing implementation: what it declares and what interest it has been told about.
struct PublisherStub {
    std::string module_id;
    std::optional<::Mapping> mapping;
    types::telemetry::SetDefinition definition;
    bool answers_definition{true};

    // recorded by the adapter
    std::vector<std::vector<std::string>> interest_calls;
    std::vector<std::string> interest_callers;
    ValueCallback on_update;

    /// \brief Publishes \p values as an update, the way the publisher framework would.
    void publish(const json::object_t& values) const {
        json update{{"module_id", module_id},
                    {"module_type", definition.module_type},
                    {"set", definition.set},
                    {"timestamp", "2026-09-09T10:41:07Z"},
                    {"values", values}};
        if (mapping.has_value()) {
            update["mapping"] = json{{"evse", mapping->evse}};
        }
        on_update(update);
    }
};

/// \brief A ModuleAdapter that routes calls and subscriptions to the PublisherStub behind the slot.
struct SlotAdapter : public module::stub::QuietModuleAdapterStub {
    std::vector<PublisherStub>* publishers{nullptr};

    Result call_fn(const Requirement& req, const std::string& cmd, Parameters args) override {
        auto& publisher = publishers->at(req.index);
        if (cmd == "get_definition") {
            if (not publisher.answers_definition) {
                throw std::runtime_error("no definition");
            }
            return json(publisher.definition);
        }
        if (cmd == "set_interest") {
            publisher.interest_callers.push_back(args.at("module_id"));
            publisher.interest_calls.push_back(args.at("entries").get<std::vector<std::string>>());
            return std::nullopt;
        }
        throw std::runtime_error("unexpected command " + cmd);
    }

    void subscribe_fn(const Requirement& req, const std::string& var, ValueCallback callback) override {
        if (var == "update") {
            publishers->at(req.index).on_update = std::move(callback);
        }
    }
};

/// \brief A sink with \p publishers wired to it, slot index following vector order.
struct Station {
    std::vector<PublisherStub> publishers;
    SlotAdapter adapter;
    Slots slots;

    explicit Station(std::vector<PublisherStub> stubs) : publishers(std::move(stubs)) {
        adapter.publishers = &publishers;
        for (std::size_t index = 0; index < publishers.size(); ++index) {
            slots.push_back(std::make_unique<telemetryIntf>(&adapter, Requirement{"telemetry", index},
                                                            publishers.at(index).module_id,
                                                            publishers.at(index).mapping));
        }
    }
};

/// \returns a set definition with \p entries, all of them numbers
inline types::telemetry::SetDefinition definition_of(const std::string& module_type, const std::string& set,
                                                     const std::vector<std::string>& entries) {
    types::telemetry::SetDefinition definition;
    definition.module_type = module_type;
    definition.set = set;
    definition.description = "test set";
    for (const auto& name : entries) {
        types::telemetry::EntryDefinition entry;
        entry.name = name;
        entry.description = "test entry";
        entry.type = types::telemetry::EntryType::number;
        definition.entries.push_back(entry);
    }
    return definition;
}

} // namespace Everest::telemetry::test
