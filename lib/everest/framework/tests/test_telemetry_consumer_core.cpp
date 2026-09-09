// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/consumer_core.hpp>

#include <tests/telemetry_harness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using namespace Everest::telemetry;
using namespace Everest::telemetry::tests;

TEST_CASE("the whole value flow arrives over one subscription", "[telemetry][consumer]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);

    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};
    PublisherStub modem{bus, "modem_1", "QuectelEC25"};

    CapturingSink sink;
    const auto subscription = core.subscribe(Filter{}, sink.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    modem.publish("livedata", Values{{"rssi_dBm", std::int64_t{-71}}});

    CHECK(bus.handler_count() == 1); // filters are matched in process, not at the broker
    CHECK(sink.count() == 2);
    CHECK(core.statistics().messages_received == 2);
    CHECK(core.statistics().dispatches == 2);
    CHECK(core.statistics().parse_errors == 0);
}

TEST_CASE("each subscriber sees only what its filter matches", "[telemetry][consumer]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);

    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};
    PublisherStub modem{bus, "modem_1", "QuectelEC25"};

    Filter powermeter_filter;
    powermeter_filter.module_type = "LemDCBM400600";
    Filter diagnostics_filter;
    diagnostics_filter.set = "diagnostics";

    CapturingSink powermeter_sink;
    CapturingSink diagnostics_sink;
    const auto first = core.subscribe(powermeter_filter, powermeter_sink.callback());
    const auto second = core.subscribe(diagnostics_filter, diagnostics_sink.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    powermeter.publish("diagnostics", Values{{"serial_number", std::string{"LEM-0042"}}});
    modem.publish("livedata", Values{{"rssi_dBm", std::int64_t{-71}}});

    CHECK(powermeter_sink.count() == 2);
    CHECK(diagnostics_sink.count() == 1);
    CHECK(diagnostics_sink.last().set == "diagnostics");
    CHECK(core.statistics().dispatches == 3);
    CHECK(core.statistics().unmatched_messages == 1); // the modem matched no filter
}

TEST_CASE("an envelope nobody subscribed for is counted", "[telemetry][consumer]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);
    PublisherStub modem{bus, "modem_1", "QuectelEC25"};

    Filter filter;
    filter.module_id = "powermeter_1";
    CapturingSink sink;
    const auto subscription = core.subscribe(filter, sink.callback());

    modem.publish("livedata", Values{{"rssi_dBm", std::int64_t{-71}}});

    CHECK(sink.empty());
    CHECK(core.statistics().unmatched_messages == 1);
    CHECK(core.statistics().dispatches == 0);
}

TEST_CASE("dropping the subscription handle stops delivery", "[telemetry][consumer]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);
    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};

    CapturingSink sink;
    {
        const auto subscription = core.subscribe(Filter{}, sink.callback());
        CHECK(subscription.active());
        CHECK(core.subscription_count() == 1);
        powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    }

    CHECK(core.subscription_count() == 0);
    powermeter.publish("livedata", Values{{"temperature_C", 42.0}});

    CHECK(sink.count() == 1);
    CHECK(core.statistics().messages_received == 2);
    CHECK(core.statistics().unmatched_messages == 1);

    SECTION("an explicit reset is idempotent") {
        auto subscription = core.subscribe(Filter{}, sink.callback());
        subscription.reset();
        subscription.reset();
        CHECK_FALSE(subscription.active());
        CHECK(core.subscription_count() == 0);
    }
}

TEST_CASE("a subscription handle may outlive its core", "[telemetry][consumer]") {
    CapturingSink sink;
    std::optional<ConsumerCore::Subscription> subscription;

    {
        ConsumerCore core;
        subscription = core.subscribe(Filter{}, sink.callback());
        CHECK(subscription->active());
    }

    CHECK_FALSE(subscription->active());
    subscription->reset(); // must not touch the destroyed core
}

TEST_CASE("a broken producer cannot take the consumer down", "[telemetry][consumer]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);
    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};

    std::vector<ParseError> reported;
    core.set_parse_error_handler(
        [&reported](std::string_view, const ParseResult& result) { reported.push_back(result.error); });

    CapturingSink sink;
    const auto subscription = core.subscribe(Filter{}, sink.callback());

    powermeter.publish_payload("livedata", nlohmann::json::array({1, 2}));
    powermeter.publish_payload("livedata", nlohmann::json{{"version", 1}, {"module_id", "powermeter_1"}});
    powermeter.publish_payload("livedata", nlohmann::json{{"version", 99},
                                                          {"module_id", "powermeter_1"},
                                                          {"module_type", "LemDCBM400600"},
                                                          {"set", "livedata"},
                                                          {"timestamp", "2026-08-12T10:41:07Z"},
                                                          {"values", nlohmann::json::object()}});
    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});

    CHECK(sink.count() == 1); // the healthy message still arrives
    CHECK(core.statistics().messages_received == 4);
    CHECK(core.statistics().parse_errors == 3);
    REQUIRE(reported.size() == 3);
    CHECK(reported.at(0) == ParseError::NotAnObject);
    CHECK(reported.at(1) == ParseError::MissingField);
    CHECK(reported.at(2) == ParseError::UnsupportedVersion);
}

TEST_CASE("raw bytes that are not json are counted as a parse error", "[telemetry][consumer]") {
    ConsumerCore core;
    CapturingSink sink;
    const auto subscription = core.subscribe(Filter{}, sink.callback());

    core.handle_message("everest-telemetry/v1/powermeter_1/livedata", std::string_view{"{\"version\": "});

    CHECK(sink.empty());
    CHECK(core.statistics().messages_received == 1);
    CHECK(core.statistics().parse_errors == 1);
}

TEST_CASE("validation against the catalog is opt in", "[telemetry][consumer][validation]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);
    core.set_definitions(powermeter_definitions());
    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};

    CapturingSink sink;
    const auto subscription = core.subscribe(Filter{}, sink.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}, {"undeclared", 1.0}});

    CHECK(sink.count() == 1);
    CHECK(sink.last().values.size() == 2); // unchecked, so nothing is removed
    CHECK(core.statistics().invalid_messages == 0);
}

TEST_CASE("a validating consumer reports findings and keeps dispatching", "[telemetry][consumer][validation]") {
    FakeBus bus;
    ConsumerCore core{ConsumerCore::Options{true, false}};
    connect(core, bus);
    core.set_definitions(powermeter_definitions());
    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};

    std::vector<ValidationFinding> findings;
    core.set_validation_handler([&findings](const Envelope&, const ValidationResult& result) {
        findings.insert(findings.end(), result.findings.begin(), result.findings.end());
    });

    CapturingSink sink;
    const auto subscription = core.subscribe(Filter{}, sink.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 150.0}, {"undeclared", 1.0}});

    CHECK(sink.count() == 1);
    CHECK(sink.last().values.size() == 2);
    CHECK(core.statistics().invalid_messages == 1);
    CHECK(core.statistics().entries_dropped == 0);
    REQUIRE(findings.size() == 2);
}

TEST_CASE("invalid values can be dropped instead of forwarded", "[telemetry][consumer][validation]") {
    FakeBus bus;
    ConsumerCore core{ConsumerCore::Options{true, true}};
    connect(core, bus);
    core.set_definitions(powermeter_definitions());
    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};

    CapturingSink sink;
    const auto subscription = core.subscribe(Filter{}, sink.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}, {"undeclared", 1.0}});

    REQUIRE(sink.count() == 1);
    CHECK(sink.last().values.size() == 1);
    CHECK(sink.last().values.count("temperature_C") == 1);
    CHECK(core.statistics().entries_dropped == 1);

    SECTION("an envelope with nothing left is not dispatched") {
        sink.clear();
        powermeter.publish("livedata", Values{{"undeclared", 1.0}});

        CHECK(sink.empty());
        CHECK(core.statistics().messages_dropped == 1);
        CHECK(core.statistics().entries_dropped == 2);
    }

    SECTION("an envelope from an undeclared module is dropped whole") {
        sink.clear();
        PublisherStub unknown{bus, "powermeter_2", "LemDCBM400600"};
        unknown.publish("livedata", Values{{"temperature_C", 41.2}, {"frequency_Hz", 49.98}});

        CHECK(sink.empty());
        CHECK(core.statistics().messages_dropped == 1);
        CHECK(core.statistics().entries_dropped == 3);
    }
}

TEST_CASE("devices behind one module are addressable", "[telemetry][consumer][addressing]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);

    // One driver module, 24 converters. EVSE id cannot tell them apart, and with dynamic power
    // allocation they are not EVSE-specific at all.
    PublisherStub converters{bus, "power_supply_1", "Huawei_V100R023C10"};

    Filter seventh;
    seventh.instance = "converter_7";
    CapturingSink sink;
    const auto subscription = core.subscribe(seventh, sink.callback());

    for (int i = 1; i <= 24; ++i) {
        PublisherStub::Attributes attributes;
        attributes.instance = "converter_" + std::to_string(i);
        converters.publish("livedata", Values{{"temperature_C", 40.0 + i}}, attributes);
    }

    REQUIRE(sink.count() == 1);
    CHECK(sink.last().instance == "converter_7");
    CHECK(sink.last().address().to_string() == "power_supply_1/livedata/converter_7");
    CHECK(std::get<double>(sink.last().values.at("temperature_C")) == 47.0);
    CHECK(core.statistics().messages_received == 24);
}

TEST_CASE("the fake bus routes the way the broker does", "[telemetry][harness]") {
    CHECK(FakeBus::topic_matches("everest-telemetry/v1/+/+", "everest-telemetry/v1/powermeter_1/livedata"));
    CHECK_FALSE(FakeBus::topic_matches("everest-telemetry/v1/+/+", "everest-telemetry/v1/powermeter_1"));
    CHECK_FALSE(
        FakeBus::topic_matches("everest-telemetry/v1/+/+", "everest-telemetry/v1/powermeter_1/livedata/extra"));
    CHECK_FALSE(FakeBus::topic_matches("everest-telemetry/v1/+/+", "everest/powermeter_1/var"));
    CHECK(FakeBus::topic_matches("everest-telemetry/v1/#", "everest-telemetry/v1/powermeter_1/livedata"));
    CHECK(FakeBus::topic_matches("everest-telemetry/v1/powermeter_1/+",
                                 "everest-telemetry/v1/powermeter_1/diagnostics"));
    CHECK_FALSE(FakeBus::topic_matches("everest-telemetry/v1/powermeter_1/+",
                                       "everest-telemetry/v1/powermeter_2/diagnostics"));
}

TEST_CASE("a subscriber may unsubscribe from inside its own callback", "[telemetry][consumer]") {
    FakeBus bus;
    ConsumerCore core;
    connect(core, bus);
    PublisherStub powermeter{bus, "powermeter_1", "LemDCBM400600"};

    std::optional<ConsumerCore::Subscription> subscription;
    std::size_t seen = 0;
    subscription = core.subscribe(Filter{}, [&](const Envelope&) {
        ++seen;
        subscription->reset();
    });

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    powermeter.publish("livedata", Values{{"temperature_C", 41.4}});

    CHECK(seen == 1);
    CHECK(core.subscription_count() == 0);
}
