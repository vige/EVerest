// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/consumer.hpp>

#include <tests/telemetry_harness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace Everest::telemetry;
using namespace Everest::telemetry::tests;

TEST_CASE("a filter is the broker subscription", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    CapturingSink sink;

    SECTION("no filter subscribes to the whole value flow") {
        const auto subscription = consumer.subscribe(Filter{}, sink.callback());
        CHECK(subscription.topic() == "everest-telemetry/v1/+/+");
        CHECK(broker.subscribed_topics() == std::vector<std::string>{"everest-telemetry/v1/+/+"});
    }

    SECTION("a set filter subscribes to that set across all producers") {
        Filter filter;
        filter.set = "livedata";
        const auto subscription = consumer.subscribe(filter, sink.callback());
        CHECK(subscription.topic() == "everest-telemetry/v1/+/livedata");
    }

    SECTION("a module filter subscribes to one producer") {
        Filter filter;
        filter.module_id = "powermeter_1";
        const auto subscription = consumer.subscribe(filter, sink.callback());
        CHECK(subscription.topic() == "everest-telemetry/v1/powermeter_1/+");
    }

    SECTION("both members subscribe to a single topic") {
        Filter filter;
        filter.module_id = "powermeter_1";
        filter.set = "diagnostics";
        const auto subscription = consumer.subscribe(filter, sink.callback());
        CHECK(subscription.topic() == "everest-telemetry/v1/powermeter_1/diagnostics");
    }
}

TEST_CASE("the broker decides what a consumer sees", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};

    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};
    PublisherStub modem{broker, "modem_1", "QuectelEC25"};

    Filter livedata;
    livedata.set = "livedata";
    CapturingSink sink;
    const auto subscription = consumer.subscribe(livedata, sink.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    modem.publish("livedata", Values{{"rssi_dBm", std::int64_t{-71}}});
    const auto diagnostics_delivered =
        powermeter.publish("diagnostics", Values{{"serial_number", std::string{"LEM-0042"}}});

    CHECK(sink.count() == 2);
    CHECK(diagnostics_delivered == 0); // the topic never matched, so nothing was handed over
    CHECK(sink.envelopes.at(0).module_id == "powermeter_1");
    CHECK(sink.envelopes.at(1).module_id == "modem_1");
}

TEST_CASE("two subscriptions are two broker subscriptions", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};

    Filter livedata;
    livedata.set = "livedata";
    Filter diagnostics;
    diagnostics.set = "diagnostics";

    CapturingSink fast;
    CapturingSink slow;
    const auto first = consumer.subscribe(livedata, fast.callback());
    const auto second = consumer.subscribe(diagnostics, slow.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    powermeter.publish("diagnostics", Values{{"serial_number", std::string{"LEM-0042"}}});

    CHECK(broker.subscription_count() == 2);
    CHECK(fast.count() == 1);
    CHECK(slow.count() == 1);
    CHECK(slow.last().set == "diagnostics");
}

TEST_CASE("identical filters get their own subscription each", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};

    CapturingSink ocpp;
    CapturingSink async_api;
    const auto first = consumer.subscribe(Filter{}, ocpp.callback());
    const auto second = consumer.subscribe(Filter{}, async_api.callback());

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});

    CHECK(ocpp.count() == 1);
    CHECK(async_api.count() == 1);
}

TEST_CASE("dropping the handle unsubscribes at the broker", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};
    CapturingSink sink;

    {
        const auto subscription = consumer.subscribe(Filter{}, sink.callback());
        CHECK(subscription.active());
        CHECK(broker.subscription_count() == 1);
        powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    }

    CHECK(broker.subscription_count() == 0);
    const auto delivered = powermeter.publish("livedata", Values{{"temperature_C", 42.0}});

    CHECK(delivered == 0);
    CHECK(sink.count() == 1);

    SECTION("an explicit reset is idempotent") {
        auto subscription = consumer.subscribe(Filter{}, sink.callback());
        subscription.reset();
        subscription.reset();
        CHECK_FALSE(subscription.active());
        CHECK(broker.subscription_count() == 0);
    }
}

TEST_CASE("a subscription can be moved", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};
    CapturingSink sink;

    auto subscription = consumer.subscribe(Filter{}, sink.callback());
    auto moved = std::move(subscription);

    CHECK_FALSE(subscription.active());
    CHECK(moved.active());
    CHECK(broker.subscription_count() == 1);

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    CHECK(sink.count() == 1);
}

TEST_CASE("an envelope arrives whole", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub converters{broker, "power_supply_1", "Huawei_V100R023C10"};
    CapturingSink sink;
    const auto subscription = consumer.subscribe(Filter{}, sink.callback());

    PublisherStub::Attributes attributes;
    attributes.instance = "converter_7";
    attributes.mapping = EvseMapping{1, std::nullopt};
    attributes.seq = 4711;
    attributes.labels = {{"slot", "B3"}};
    attributes.timestamp = "2026-08-12T10:41:07Z";
    converters.publish("livedata", Values{{"temperature_C", 41.2}, {"frequency_Hz", 49.98}}, attributes);

    REQUIRE(sink.count() == 1);
    const auto& envelope = sink.last();
    CHECK(envelope.module_id == "power_supply_1"); // from the topic
    CHECK(envelope.set == "livedata");             // from the topic
    CHECK(envelope.module_type == "Huawei_V100R023C10");
    CHECK(envelope.instance == "converter_7");
    CHECK(envelope.timestamp == "2026-08-12T10:41:07Z");
    REQUIRE(envelope.mapping.has_value());
    CHECK(envelope.mapping->evse == 1);
    CHECK(envelope.seq == 4711u);
    CHECK(envelope.labels.at("slot") == "B3");
    CHECK(std::get<double>(envelope.values.at("temperature_C")) == 41.2);
    CHECK(envelope.address().to_string() == "power_supply_1/livedata/converter_7");
}

TEST_CASE("devices behind one module stay apart", "[telemetry][consumer][addressing]") {
    BrokerMock broker;
    Consumer consumer{broker};

    // One driver module, 24 converters. EVSE id cannot tell them apart, and with dynamic power
    // allocation they are not EVSE-specific at all, so the instance travels in the payload.
    PublisherStub converters{broker, "power_supply_1", "Huawei_V100R023C10"};

    CapturingSink sink;
    const auto subscription = consumer.subscribe(Filter{}, sink.callback());

    for (int i = 1; i <= 24; ++i) {
        PublisherStub::Attributes attributes;
        attributes.instance = "converter_" + std::to_string(i);
        converters.publish("livedata", Values{{"temperature_C", 40.0 + i}}, attributes);
    }

    REQUIRE(sink.count() == 24);
    CHECK(sink.envelopes.at(6).address().to_string() == "power_supply_1/livedata/converter_7");
    CHECK(std::get<double>(sink.envelopes.at(6).values.at("temperature_C")) == 47.0);

    std::set<Address> addresses;
    for (const auto& envelope : sink.envelopes) {
        addresses.insert(envelope.address());
    }
    CHECK(addresses.size() == 24);
}

TEST_CASE("a payload that cannot be read is ignored", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};
    CapturingSink sink;
    const auto subscription = consumer.subscribe(Filter{}, sink.callback());

    powermeter.publish_payload("livedata", nlohmann::json::array({1, 2}));
    powermeter.publish_payload("livedata", nlohmann::json{{"timestamp", "2026-08-12T10:41:07Z"}});
    powermeter.publish_payload("livedata", nlohmann::json{{"values", "not an object"}});
    powermeter.publish_payload("livedata", nlohmann::json{{"version", 99}, {"values", nlohmann::json::object()}});
    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});

    CHECK(sink.count() == 1); // the healthy message still arrives
}

TEST_CASE("values arrive as published, unvalidated", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    consumer.set_definitions(powermeter_definitions());
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};
    CapturingSink sink;
    const auto subscription = consumer.subscribe(Filter{}, sink.callback());

    // Outside the declared -40..120, and an entry the catalog does not know: the producer side is
    // where a declaration is enforced, so both reach the consumer untouched.
    powermeter.publish("livedata", Values{{"temperature_C", 150.0}, {"undeclared", std::int64_t{1}}});

    REQUIRE(sink.count() == 1);
    CHECK(sink.last().values.size() == 2);
    CHECK(std::get<double>(sink.last().values.at("temperature_C")) == 150.0);

    SECTION("the catalog is still there for types and units") {
        const auto* entry = find_entry(consumer.definitions(), "powermeter_1", "livedata", "temperature_C");
        REQUIRE(entry != nullptr);
        CHECK(entry->type == EntryType::Number);
        CHECK(entry->unit == "Celsius");
    }
}

TEST_CASE("a subscriber may unsubscribe from inside its own callback", "[telemetry][consumer]") {
    BrokerMock broker;
    Consumer consumer{broker};
    PublisherStub powermeter{broker, "powermeter_1", "LemDCBM400600"};

    std::optional<Consumer::Subscription> subscription;
    std::size_t seen = 0;
    subscription = consumer.subscribe(Filter{}, [&](const Envelope&) {
        ++seen;
        subscription->reset();
    });

    powermeter.publish("livedata", Values{{"temperature_C", 41.2}});
    powermeter.publish("livedata", Values{{"temperature_C", 41.4}});

    CHECK(seen == 1);
    CHECK(broker.subscription_count() == 0);
}

TEST_CASE("delivery follows MQTT wildcard semantics", "[telemetry][harness]") {
    CHECK(BrokerMock::topic_matches("everest-telemetry/v1/+/+", "everest-telemetry/v1/powermeter_1/livedata"));
    CHECK(BrokerMock::topic_matches("everest-telemetry/v1/+/livedata",
                                    "everest-telemetry/v1/powermeter_1/livedata"));
    CHECK_FALSE(BrokerMock::topic_matches("everest-telemetry/v1/+/livedata",
                                          "everest-telemetry/v1/powermeter_1/diagnostics"));
    CHECK_FALSE(BrokerMock::topic_matches("everest-telemetry/v1/+/+", "everest-telemetry/v1/powermeter_1"));
    CHECK_FALSE(
        BrokerMock::topic_matches("everest-telemetry/v1/+/+", "everest-telemetry/v1/powermeter_1/livedata/x"));
    CHECK_FALSE(BrokerMock::topic_matches("everest-telemetry/v1/+/+", "everest/powermeter_1/var"));
}
