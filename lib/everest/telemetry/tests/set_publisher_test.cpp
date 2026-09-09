// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <gtest/gtest.h>

#include <everest/telemetry/set_publisher.hpp>

namespace Everest::telemetry {

namespace {

types::telemetry::SetDefinition livedata() {
    types::telemetry::SetDefinition definition;
    definition.module_type = "TelemetryProducerExample";
    definition.set = "livedata";
    definition.description = "test set";
    for (const auto& name : {"temperature_C", "frequency_Hz"}) {
        types::telemetry::EntryDefinition entry;
        entry.name = name;
        entry.description = "test entry";
        entry.type = types::telemetry::EntryType::number;
        definition.entries.push_back(entry);
    }
    return definition;
}

/// \brief A publisher that records what it would have put on the bus.
struct Recorder {
    std::vector<types::telemetry::Update> sent;
    SetPublisher publisher;

    Recorder() :
        publisher(livedata(), [this](const types::telemetry::Update& update) { sent.push_back(update); }) {
        publisher.set_module_id("powermeter_1");
    }
};

} // namespace

TEST(SetPublisher, publishes_nothing_at_all_while_nobody_is_interested) {
    // This is what lets a module call publish() unconditionally: no guard, no traffic.
    Recorder recorder;

    EXPECT_FALSE(recorder.publisher.offer({{"temperature_C", 41.2}, {"frequency_Hz", 49.98}}));
    EXPECT_TRUE(recorder.sent.empty());
    EXPECT_TRUE(recorder.publisher.wanted().empty());
}

TEST(SetPublisher, an_offer_made_while_silent_still_reaches_the_next_subscriber) {
    // The cache holds every declared value, wanted or not, so interest declared later is answered
    // with a real snapshot rather than a hole.
    Recorder recorder;
    recorder.publisher.offer({{"temperature_C", 41.2}});

    recorder.publisher.set_interest("forwarder", {"temperature_C"});

    ASSERT_EQ(recorder.sent.size(), 1u);
    EXPECT_EQ(recorder.sent.at(0).values.at("temperature_C"), 41.2);
}

TEST(SetPublisher, only_the_wanted_entries_are_published) {
    Recorder recorder;
    recorder.publisher.set_interest("forwarder", {"temperature_C"});
    recorder.sent.clear();

    EXPECT_TRUE(recorder.publisher.offer({{"temperature_C", 41.3}, {"frequency_Hz", 49.99}}));
    ASSERT_EQ(recorder.sent.size(), 1u);
    EXPECT_EQ(recorder.sent.at(0).values.size(), 1u);
    EXPECT_TRUE(recorder.sent.at(0).values.count("temperature_C"));
}

TEST(SetPublisher, an_unchanged_value_is_silent) {
    Recorder recorder;
    recorder.publisher.set_interest("forwarder", {"temperature_C"});
    recorder.publisher.offer({{"temperature_C", 41.3}});
    recorder.sent.clear();

    EXPECT_FALSE(recorder.publisher.offer({{"temperature_C", 41.3}}));
    EXPECT_TRUE(recorder.sent.empty());
}

TEST(SetPublisher, an_undeclared_entry_is_dropped_rather_than_published) {
    Recorder recorder;
    recorder.publisher.set_interest("forwarder", {"temperature_C"});
    recorder.sent.clear();

    EXPECT_FALSE(recorder.publisher.offer({{"not_declared", 1.0}}));
    EXPECT_TRUE(recorder.sent.empty());
}

TEST(SetPublisher, withdrawing_the_last_interest_makes_it_silent_again) {
    Recorder recorder;
    recorder.publisher.set_interest("forwarder", {"temperature_C"});
    recorder.publisher.set_interest("forwarder", {});
    recorder.sent.clear();

    EXPECT_FALSE(recorder.publisher.offer({{"temperature_C", 42.0}}));
    EXPECT_TRUE(recorder.sent.empty());
}

TEST(SetPublisher, what_is_published_is_the_union_over_subscribers) {
    Recorder recorder;
    recorder.publisher.set_interest("ocpp", {"temperature_C"});
    recorder.publisher.set_interest("forwarder", {"frequency_Hz"});
    recorder.sent.clear();

    EXPECT_TRUE(recorder.publisher.offer({{"temperature_C", 41.4}, {"frequency_Hz", 50.0}}));
    ASSERT_EQ(recorder.sent.size(), 1u);
    EXPECT_EQ(recorder.sent.at(0).values.size(), 2u);
    EXPECT_EQ(recorder.publisher.wanted(), (std::set<std::string>{"temperature_C", "frequency_Hz"}));
}

} // namespace Everest::telemetry
