// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <gtest/gtest.h>

#include "telemetry_harness.hpp"

namespace Everest::telemetry {

using test::definition_of;
using test::PublisherStub;
using test::Station;

namespace {

std::vector<PublisherStub> two_publishers() {
    PublisherStub livedata;
    livedata.module_id = "powermeter_1";
    livedata.mapping = ::Mapping{1};
    livedata.definition = definition_of("LemDCBM400600", "livedata", {"temperature_C", "frequency_Hz"});

    PublisherStub diagnostics;
    diagnostics.module_id = "powermeter_1";
    diagnostics.mapping = ::Mapping{1};
    diagnostics.definition = definition_of("LemDCBM400600", "diagnostics", {"uptime_s"});

    PublisherStub other;
    other.module_id = "modem_1";
    other.definition = definition_of("QuectelModem", "livedata", {"rssi_dBm"});

    return {livedata, diagnostics, other};
}

} // namespace

TEST(Sink, resolves_a_definition_per_slot) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    EXPECT_EQ(sink.resolve_definitions(), 3u);
    ASSERT_NE(sink.definition(0), nullptr);
    EXPECT_EQ(sink.definition(0)->set, "livedata");
    EXPECT_EQ(sink.definition(SetKey{"modem_1", "livedata"})->module_type, "QuectelModem");
    EXPECT_EQ(sink.definitions().size(), 3u);
}

TEST(Sink, a_slot_that_does_not_answer_is_skipped_not_fatal) {
    auto publishers = two_publishers();
    publishers.at(1).answers_definition = false;
    Station station{publishers};
    Sink sink{station.slots, "forwarder"};

    EXPECT_EQ(sink.resolve_definitions(), 2u);
    EXPECT_EQ(sink.definition(1), nullptr);

    // and it is never asked for interest, because a filter cannot be evaluated against it
    EXPECT_EQ(sink.declare_interest(Filter{}), 2u);
    EXPECT_TRUE(station.publishers.at(1).interest_calls.empty());
}

TEST(Sink, an_empty_filter_declares_interest_in_every_declared_entry) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();

    EXPECT_EQ(sink.declare_interest(Filter{}), 3u);
    EXPECT_EQ(station.publishers.at(0).interest_calls.at(0),
              (std::vector<std::string>{"temperature_C", "frequency_Hz"}));
    EXPECT_EQ(station.publishers.at(0).interest_callers.at(0), "forwarder");
}

TEST(Sink, declaring_interest_resolves_the_definitions_by_itself) {
    // A sink that only wants to receive should not have to ask for the definitions first.
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    EXPECT_EQ(sink.declare_interest(Filter{}), 3u);
    EXPECT_EQ(sink.definitions().size(), 3u);
    EXPECT_EQ(station.publishers.at(0).interest_calls.size(), 1u);
}

TEST(Sink, the_definitions_are_resolved_once_however_interest_is_declared) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    sink.resolve_definitions();
    sink.declare_interest(Filter{});
    Filter narrowed;
    narrowed.set = "diagnostics";
    sink.declare_interest(narrowed);

    // get_definition is a blocking round trip per slot; asking again on every filter change would
    // make a redeclaration cost as much as a startup.
    EXPECT_EQ(station.publishers.at(0).definition_calls, 1u);
}

TEST(Sink, a_filter_on_the_set_leaves_the_other_sets_untouched) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();

    Filter filter;
    filter.set = "livedata";
    EXPECT_EQ(sink.declare_interest(filter), 2u);
    EXPECT_EQ(station.publishers.at(0).interest_calls.size(), 1u);
    EXPECT_TRUE(station.publishers.at(1).interest_calls.empty()) << "diagnostics was never wanted";
    EXPECT_EQ(station.publishers.at(2).interest_calls.size(), 1u);
}

TEST(Sink, a_filter_on_module_type_and_evse_selects_one_publisher) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();

    Filter filter;
    filter.module_type = "LemDCBM400600";
    filter.evse = 1;
    filter.set = "livedata";
    EXPECT_EQ(sink.declare_interest(filter), 1u);
    EXPECT_EQ(sink.interest(0), (std::set<std::string>{"temperature_C", "frequency_Hz"}));
    EXPECT_TRUE(sink.interest(2).empty()) << "modem_1 has no mapping, so evse cannot match";
}

TEST(Sink, a_filter_on_entries_narrows_within_the_set) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();

    Filter filter;
    filter.entries = {"temperature_C", "uptime_s"};
    EXPECT_EQ(sink.declare_interest(filter), 2u);
    EXPECT_EQ(station.publishers.at(0).interest_calls.at(0), (std::vector<std::string>{"temperature_C"}));
    EXPECT_EQ(station.publishers.at(1).interest_calls.at(0), (std::vector<std::string>{"uptime_s"}));
    EXPECT_TRUE(station.publishers.at(2).interest_calls.empty()) << "no entry of modem_1 was asked for";
}

TEST(Sink, redeclaring_withdraws_the_slots_that_no_longer_match) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();
    sink.declare_interest(Filter{});

    Filter narrowed;
    narrowed.set = "diagnostics";
    EXPECT_EQ(sink.declare_interest(narrowed), 1u);
    EXPECT_EQ(station.publishers.at(0).interest_calls.size(), 2u);
    EXPECT_TRUE(station.publishers.at(0).interest_calls.at(1).empty()) << "livedata was withdrawn";
    EXPECT_TRUE(sink.interest(0).empty());
}

TEST(Sink, a_slot_that_was_never_wanted_is_not_withdrawn) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();

    Filter filter;
    filter.set = "diagnostics";
    sink.declare_interest(filter);
    sink.declare_interest(filter);

    EXPECT_TRUE(station.publishers.at(0).interest_calls.empty()) << "an empty withdrawal is noise on the bus";
    EXPECT_EQ(station.publishers.at(1).interest_calls.size(), 2u);
}

TEST(Sink, withdraw_clears_every_slot_that_had_interest) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.resolve_definitions();
    sink.declare_interest(Filter{});

    sink.withdraw();
    for (const auto& publisher : station.publishers) {
        ASSERT_EQ(publisher.interest_calls.size(), 2u);
        EXPECT_TRUE(publisher.interest_calls.at(1).empty());
    }

    sink.withdraw();
    EXPECT_EQ(station.publishers.at(0).interest_calls.size(), 2u) << "withdraw is idempotent";
}

TEST(Sink, an_update_reaches_the_handler_with_its_attribution) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    std::vector<types::telemetry::Update> seen;
    sink.subscribe([&seen](const types::telemetry::Update& update) { seen.push_back(update); });
    sink.resolve_definitions();
    sink.declare_interest(Filter{});

    station.publishers.at(0).publish({{"temperature_C", 41.2}});

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.at(0).module_id, "powermeter_1");
    EXPECT_EQ(seen.at(0).module_type, "LemDCBM400600");
    EXPECT_EQ(seen.at(0).set, "livedata");
    ASSERT_TRUE(seen.at(0).mapping.has_value());
    EXPECT_EQ(seen.at(0).mapping->evse, 1);
    EXPECT_EQ(seen.at(0).values.at("temperature_C"), 41.2);
}

TEST(Sink, entries_outside_the_interest_are_pruned_from_the_union) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    std::vector<types::telemetry::Update> seen;
    sink.subscribe([&seen](const types::telemetry::Update& update) { seen.push_back(update); });
    sink.resolve_definitions();

    Filter filter;
    filter.entries = {"temperature_C"};
    sink.declare_interest(filter);

    // another sink asked for frequency_Hz as well, so the publisher emits the union
    station.publishers.at(0).publish({{"temperature_C", 41.2}, {"frequency_Hz", 49.98}});

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.at(0).values.size(), 1u);
    EXPECT_TRUE(seen.at(0).values.count("temperature_C"));
    EXPECT_FALSE(sink.value(SetKey{"powermeter_1", "livedata"}, "frequency_Hz").has_value());
}

TEST(Sink, an_update_with_nothing_wanted_left_is_dropped) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    std::size_t calls = 0;
    sink.subscribe([&calls](const types::telemetry::Update&) { ++calls; });
    sink.resolve_definitions();

    Filter filter;
    filter.entries = {"temperature_C"};
    sink.declare_interest(filter);

    station.publishers.at(0).publish({{"frequency_Hz", 49.98}});
    EXPECT_EQ(calls, 0u);
}

TEST(Sink, the_cache_holds_the_last_value_because_no_message_means_unchanged) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.subscribe([](const types::telemetry::Update&) {});
    sink.resolve_definitions();
    sink.declare_interest(Filter{});

    const SetKey key{"powermeter_1", "livedata"};
    station.publishers.at(0).publish({{"temperature_C", 41.2}, {"frequency_Hz", 49.98}});
    station.publishers.at(0).publish({{"temperature_C", 42.0}});

    EXPECT_EQ(sink.value(key, "temperature_C"), json(42.0)) << "overwritten by the newer update";
    EXPECT_EQ(sink.value(key, "frequency_Hz"), json(49.98)) << "absent from the second update, so unchanged";
    EXPECT_EQ(sink.values(key).size(), 2u);
    EXPECT_TRUE(sink.values(SetKey{"powermeter_1", "diagnostics"}).empty());
}

TEST(Sink, updates_of_two_sets_of_one_module_do_not_share_a_cache) {
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};
    sink.subscribe([](const types::telemetry::Update&) {});
    sink.resolve_definitions();
    sink.declare_interest(Filter{});

    station.publishers.at(0).publish({{"temperature_C", 41.2}});
    station.publishers.at(1).publish({{"uptime_s", 900.0}});

    EXPECT_EQ(sink.values(SetKey{"powermeter_1", "livedata"}).size(), 1u);
    EXPECT_EQ(sink.values(SetKey{"powermeter_1", "diagnostics"}).size(), 1u);
}

TEST(Sink, an_update_before_interest_is_declared_is_still_delivered) {
    // The publisher is what gates the flow, not the sink. If a message arrives anyway - another
    // sink's interest, a publisher that ignores the protocol - it is not dropped.
    Station station{two_publishers()};
    Sink sink{station.slots, "forwarder"};

    std::size_t calls = 0;
    sink.subscribe([&calls](const types::telemetry::Update&) { ++calls; });
    station.publishers.at(0).publish({{"temperature_C", 41.2}});

    EXPECT_EQ(calls, 1u);
}

TEST(Sink, a_sink_with_no_slots_wired_is_inert) {
    Station station{{}};
    Sink sink{station.slots, "forwarder"};
    sink.subscribe([](const types::telemetry::Update&) {});

    EXPECT_EQ(sink.resolve_definitions(), 0u);
    EXPECT_EQ(sink.declare_interest(Filter{}), 0u);
    EXPECT_NO_THROW(sink.withdraw());
    EXPECT_TRUE(sink.definitions().empty());
}

TEST(SetKey, orders_by_module_then_set) {
    EXPECT_LT((SetKey{"a", "z"}), (SetKey{"b", "a"}));
    EXPECT_LT((SetKey{"a", "a"}), (SetKey{"a", "b"}));
    EXPECT_EQ((SetKey{"a", "b"}), (SetKey{"a", "b"}));
    EXPECT_EQ((SetKey{"powermeter_1", "livedata"}).to_string(), "powermeter_1/livedata");
}

} // namespace Everest::telemetry
