// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/types.hpp>

#include <tests/telemetry_harness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace Everest::telemetry;
using namespace Everest::telemetry::tests;

namespace {

/// \brief The catalog as the ConfigService would hand it over, matching the manifest example in
/// EVerest/EVerest#2639.
nlohmann::json catalog_payload() {
    return nlohmann::json::parse(R"({
        "powermeter_1": {
            "module_type": "LemDCBM400600",
            "sets": {
                "livedata": {
                    "description": "Live electrical measurements",
                    "max_publish_rate_hz": 4,
                    "entries": {
                        "temperature_C": {"type": "number", "unit": "Celsius", "minimum": -40, "maximum": 120},
                        "frequency_Hz": {"type": "number", "unit": "Hertz"},
                        "fw_state": {"type": "string", "values_list": ["Idle", "Measuring", "Error"]}
                    }
                },
                "diagnostics": {
                    "entries": {
                        "serial_number": {"type": "string"},
                        "restarts": {"type": "integer"}
                    }
                }
            }
        }
    })");
}

Envelope livedata_envelope() {
    Envelope envelope;
    envelope.module_id = "powermeter_1";
    envelope.module_type = "LemDCBM400600";
    envelope.set = "livedata";
    envelope.timestamp = "2026-08-12T10:41:07Z";
    envelope.mapping = EvseMapping{1, std::nullopt};
    envelope.values = Values{{"temperature_C", 41.2}, {"frequency_Hz", 49.98}};
    return envelope;
}

const std::string LIVEDATA_TOPIC = "everest-telemetry/v1/powermeter_1/livedata";

} // namespace

TEST_CASE("entry types round trip through their manifest spelling", "[telemetry][types]") {
    for (const auto type : {EntryType::Boolean, EntryType::Integer, EntryType::Number, EntryType::String,
                            EntryType::Object, EntryType::Array}) {
        CHECK(entry_type_from_string(to_string(type)) == type);
    }

    SECTION("a spelling outside the vocabulary is rejected, not guessed") {
        CHECK_FALSE(entry_type_from_string("float").has_value());
        CHECK_FALSE(entry_type_from_string("").has_value());
        CHECK_FALSE(entry_type_from_string("Number").has_value());
    }
}

TEST_CASE("the catalog is shaped for the lookup in the proposal", "[telemetry][catalog]") {
    const auto definitions = catalog_payload().get<TelemetryDefinitions>();

    // Exactly the access path from the issue.
    const auto& entry = definitions.at("powermeter_1").sets.at("livedata").entries.at("temperature_C");

    CHECK(entry.type == EntryType::Number);
    CHECK(entry.unit == "Celsius");
    CHECK(entry.minimum == -40.0);
    CHECK(entry.maximum == 120.0);

    SECTION("declaration metadata survives") {
        const auto& livedata = definitions.at("powermeter_1").sets.at("livedata");
        CHECK(livedata.description == "Live electrical measurements");
        CHECK(livedata.max_publish_rate_hz == 4.0);
        CHECK(definitions.at("powermeter_1").module_type == "LemDCBM400600");
        REQUIRE(livedata.entries.at("fw_state").values_list.has_value());
        CHECK(livedata.entries.at("fw_state").values_list->size() == 3);
    }

    SECTION("a module may declare several sets") {
        CHECK(definitions.at("powermeter_1").sets.count("diagnostics") == 1);
        CHECK(definitions.at("powermeter_1").sets.at("diagnostics").entries.at("restarts").type ==
              EntryType::Integer);
    }

    SECTION("find_entry answers for every level of the path") {
        CHECK(find_entry(definitions, "powermeter_1", "livedata", "frequency_Hz") != nullptr);
        CHECK(find_entry(definitions, "powermeter_1", "livedata", "no_such_entry") == nullptr);
        CHECK(find_entry(definitions, "powermeter_1", "no_such_set", "frequency_Hz") == nullptr);
        CHECK(find_entry(definitions, "no_such_module", "livedata", "frequency_Hz") == nullptr);
    }
}

TEST_CASE("the catalog survives a serialization round trip", "[telemetry][catalog]") {
    const auto definitions = catalog_payload().get<TelemetryDefinitions>();
    const nlohmann::json serialized = definitions;
    const auto restored = serialized.get<TelemetryDefinitions>();

    const auto& original_entry = definitions.at("powermeter_1").sets.at("livedata").entries.at("fw_state");
    const auto& restored_entry = restored.at("powermeter_1").sets.at("livedata").entries.at("fw_state");

    CHECK(restored.size() == definitions.size());
    CHECK(restored_entry.type == original_entry.type);
    CHECK(restored_entry.values_list == original_entry.values_list);
    CHECK(restored.at("powermeter_1").sets.at("livedata").max_publish_rate_hz == 4.0);
}

TEST_CASE("an unknown entry type in the catalog is an error", "[telemetry][catalog]") {
    const auto payload = nlohmann::json::parse(R"({
        "powermeter_1": {"module_type": "T", "sets": {"livedata": {"entries": {"x": {"type": "decimal"}}}}}
    })");

    CHECK_THROWS(payload.get<TelemetryDefinitions>());
}

TEST_CASE("the topic carries module id and set", "[telemetry][topic]") {
    CHECK(topic_for("powermeter_1", "livedata") == LIVEDATA_TOPIC);

    const auto address = parse_topic(LIVEDATA_TOPIC);
    REQUIRE(address.has_value());
    CHECK(address->module_id == "powermeter_1");
    CHECK(address->set == "livedata");

    SECTION("anything that is not a value flow topic is refused") {
        CHECK_FALSE(parse_topic("everest/powermeter_1/livedata").has_value());
        CHECK_FALSE(parse_topic("everest-telemetry/v1/powermeter_1").has_value());
        CHECK_FALSE(parse_topic("everest-telemetry/v1/powermeter_1/livedata/extra").has_value());
        CHECK_FALSE(parse_topic("everest-telemetry/v1//livedata").has_value());
        CHECK_FALSE(parse_topic("").has_value());
    }
}

TEST_CASE("a filter becomes a topic to subscribe to", "[telemetry][filter]") {
    CHECK(Filter{}.topic_pattern() == "everest-telemetry/v1/+/+");
    CHECK(TOPIC_WILDCARD == Filter{}.topic_pattern());

    Filter by_set;
    by_set.set = "livedata";
    CHECK(by_set.topic_pattern() == "everest-telemetry/v1/+/livedata");

    Filter by_module;
    by_module.module_id = "powermeter_1";
    CHECK(by_module.topic_pattern() == "everest-telemetry/v1/powermeter_1/+");

    Filter both;
    both.module_id = "powermeter_1";
    both.set = "diagnostics";
    CHECK(both.topic_pattern() == "everest-telemetry/v1/powermeter_1/diagnostics");
}

TEST_CASE("an envelope survives a round trip over the wire", "[telemetry][envelope]") {
    auto envelope = livedata_envelope();
    envelope.instance = "converter_7";
    envelope.seq = 42;
    envelope.labels = {{"slot", "B3"}};

    const auto payload = to_payload(envelope);
    const auto restored = read_envelope(topic_for(envelope.module_id, envelope.set), payload);

    REQUIRE(restored.has_value());
    CHECK(restored->version == ENVELOPE_VERSION);
    CHECK(restored->module_id == "powermeter_1");
    CHECK(restored->set == "livedata");
    CHECK(restored->module_type == "LemDCBM400600");
    CHECK(restored->instance == "converter_7");
    CHECK(restored->timestamp == "2026-08-12T10:41:07Z");
    REQUIRE(restored->mapping.has_value());
    CHECK(restored->mapping->evse == 1);
    CHECK_FALSE(restored->mapping->connector.has_value());
    CHECK(restored->seq == 42u);
    CHECK(restored->labels.at("slot") == "B3");
    CHECK(std::get<double>(restored->values.at("temperature_C")) == 41.2);

    SECTION("module id and set are not repeated in the payload") {
        CHECK(payload.count("module_id") == 0);
        CHECK(payload.count("set") == 0);
    }
}

TEST_CASE("the topic is authoritative for the identity", "[telemetry][envelope]") {
    auto payload = to_payload(livedata_envelope());
    payload["module_id"] = "somebody_else";
    payload["set"] = "diagnostics";

    const auto envelope = read_envelope(LIVEDATA_TOPIC, payload);

    REQUIRE(envelope.has_value());
    CHECK(envelope->module_id == "powermeter_1");
    CHECK(envelope->set == "livedata");
}

TEST_CASE("values keep the type they arrived with", "[telemetry][envelope]") {
    auto envelope = livedata_envelope();
    envelope.values = Values{{"fw_state", std::string{"Measuring"}},
                             {"restarts", std::int64_t{3}},
                             {"healthy", true},
                             {"temperature_C", 41.2},
                             {"raw", nlohmann::json{{"registers", {1, 2, 3}}}}};

    const auto restored = read_envelope(LIVEDATA_TOPIC, to_payload(envelope));
    REQUIRE(restored.has_value());
    const auto& values = restored->values;

    CHECK(type_of(values.at("fw_state")) == EntryType::String);
    CHECK(type_of(values.at("restarts")) == EntryType::Integer);
    CHECK(type_of(values.at("healthy")) == EntryType::Boolean);
    CHECK(type_of(values.at("temperature_C")) == EntryType::Number);
    CHECK(type_of(values.at("raw")) == EntryType::Object);
    CHECK(std::get<nlohmann::json>(values.at("raw")).at("registers").size() == 3);
}

TEST_CASE("a partial publish carries only what changed", "[telemetry][envelope]") {
    auto envelope = livedata_envelope();
    envelope.values = Values{{"temperature_C", 43.9}};

    const auto restored = read_envelope(LIVEDATA_TOPIC, to_payload(envelope));

    REQUIRE(restored.has_value());
    CHECK(restored->values.size() == 1);
    CHECK(restored->values.count("frequency_Hz") == 0);

    SECTION("an envelope with no values at all is still an envelope") {
        envelope.values.clear();
        envelope.seq = 7;
        const auto empty = read_envelope(LIVEDATA_TOPIC, to_payload(envelope));
        REQUIRE(empty.has_value());
        CHECK(empty->values.empty());
        CHECK(empty->seq == 7u);
    }
}

TEST_CASE("unknown payload fields are ignored", "[telemetry][envelope]") {
    auto payload = to_payload(livedata_envelope());
    payload["future_field"] = "from a newer producer";

    CHECK(read_envelope(LIVEDATA_TOPIC, payload).has_value());
}

TEST_CASE("a payload that cannot be read yields nothing", "[telemetry][envelope]") {
    SECTION("not an object") {
        CHECK_FALSE(read_envelope(LIVEDATA_TOPIC, nlohmann::json::array({1, 2})).has_value());
    }

    SECTION("no values object") {
        auto payload = to_payload(livedata_envelope());
        payload.erase("values");
        CHECK_FALSE(read_envelope(LIVEDATA_TOPIC, payload).has_value());

        payload["values"] = "not an object";
        CHECK_FALSE(read_envelope(LIVEDATA_TOPIC, payload).has_value());
    }

    SECTION("an envelope version this consumer does not understand") {
        auto payload = to_payload(livedata_envelope());
        payload["version"] = 2;
        CHECK_FALSE(read_envelope(LIVEDATA_TOPIC, payload).has_value());
    }

    SECTION("a topic outside the value flow") {
        CHECK_FALSE(read_envelope("everest/powermeter_1/var", to_payload(livedata_envelope())).has_value());
    }

    SECTION("optional fields of the wrong type are skipped, not fatal") {
        auto payload = to_payload(livedata_envelope());
        payload["instance"] = 7;
        payload["seq"] = "later";
        payload["mapping"] = "evse 1";

        const auto envelope = read_envelope(LIVEDATA_TOPIC, payload);
        REQUIRE(envelope.has_value());
        CHECK_FALSE(envelope->instance.has_value());
        CHECK_FALSE(envelope->seq.has_value());
        CHECK_FALSE(envelope->mapping.has_value());
        CHECK(envelope->values.size() == 2);
    }
}

TEST_CASE("an address distinguishes devices behind one module", "[telemetry][addressing]") {
    auto first = livedata_envelope();
    first.instance = "converter_1";
    auto second = livedata_envelope();
    second.instance = "converter_2";
    const auto unaddressed = livedata_envelope();

    CHECK(first.address().to_string() == "powermeter_1/livedata/converter_1");
    CHECK(unaddressed.address().to_string() == "powermeter_1/livedata");
    CHECK_FALSE(first.address() == second.address());
    CHECK_FALSE(first.address() == unaddressed.address());
    CHECK((first.address() < second.address() or second.address() < first.address()));
}
