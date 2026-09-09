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
    envelope.mapping = Mapping{1, std::nullopt};
    envelope.values = Values{{"temperature_C", 41.2}, {"frequency_Hz", 49.98}};
    return envelope;
}

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

TEST_CASE("topics carry module id and set", "[telemetry][topic]") {
    CHECK(topic_for("powermeter_1", "livedata") == "everest-telemetry/v1/powermeter_1/livedata");

    const auto address = parse_topic("everest-telemetry/v1/powermeter_1/livedata");
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

TEST_CASE("an envelope survives a round trip over the wire", "[telemetry][envelope]") {
    auto envelope = livedata_envelope();
    envelope.instance = "converter_7";
    envelope.seq = 42;
    envelope.labels = {{"slot", "B3"}};

    const auto payload = to_payload(envelope);
    const auto parsed = parse_envelope(topic_for(envelope.module_id, envelope.set), payload);

    REQUIRE(parsed.ok());
    const auto& restored = *parsed.envelope;
    CHECK(restored.version == ENVELOPE_VERSION);
    CHECK(restored.module_id == "powermeter_1");
    CHECK(restored.module_type == "LemDCBM400600");
    CHECK(restored.set == "livedata");
    CHECK(restored.instance == "converter_7");
    CHECK(restored.timestamp == "2026-08-12T10:41:07Z");
    REQUIRE(restored.mapping.has_value());
    CHECK(restored.mapping->evse == 1);
    CHECK_FALSE(restored.mapping->connector.has_value());
    CHECK(restored.seq == 42u);
    CHECK(restored.labels.at("slot") == "B3");
    CHECK(std::get<double>(restored.values.at("temperature_C")) == 41.2);
}

TEST_CASE("values keep the type they arrived with", "[telemetry][envelope]") {
    auto envelope = livedata_envelope();
    envelope.values = Values{{"fw_state", std::string{"Measuring"}},
                             {"restarts", std::int64_t{3}},
                             {"healthy", true},
                             {"temperature_C", 41.2},
                             {"raw", nlohmann::json{{"registers", {1, 2, 3}}}}};

    const auto parsed = parse_envelope(topic_for(envelope.module_id, envelope.set), to_payload(envelope));
    REQUIRE(parsed.ok());
    const auto& values = parsed.envelope->values;

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

    const auto parsed = parse_envelope(topic_for(envelope.module_id, envelope.set), to_payload(envelope));

    REQUIRE(parsed.ok());
    CHECK(parsed.envelope->values.size() == 1);
    CHECK(parsed.envelope->values.count("frequency_Hz") == 0);

    SECTION("an envelope with no values at all is still an envelope") {
        envelope.values.clear();
        envelope.seq = 7;
        const auto empty = parse_envelope(topic_for(envelope.module_id, envelope.set), to_payload(envelope));
        REQUIRE(empty.ok());
        CHECK(empty.envelope->values.empty());
        CHECK(empty.envelope->seq == 7u);
    }
}

TEST_CASE("unknown envelope fields are ignored", "[telemetry][envelope]") {
    auto payload = to_payload(livedata_envelope());
    payload["future_field"] = "from a newer producer";

    const auto parsed = parse_envelope("everest-telemetry/v1/powermeter_1/livedata", payload);

    CHECK(parsed.ok());
}

TEST_CASE("a malformed payload is reported, never guessed", "[telemetry][envelope]") {
    const std::string topic = "everest-telemetry/v1/powermeter_1/livedata";

    SECTION("not an object") {
        CHECK(parse_envelope(topic, nlohmann::json::array({1, 2})).error == ParseError::NotAnObject);
    }

    SECTION("not json at all") {
        CHECK(parse_envelope(topic, std::string_view{"{not json"}).error == ParseError::NotJson);
    }

    SECTION("an envelope version this consumer does not understand") {
        auto payload = to_payload(livedata_envelope());
        payload["version"] = 2;
        CHECK(parse_envelope(topic, payload).error == ParseError::UnsupportedVersion);
    }

    SECTION("a missing required field names the field") {
        for (const auto* field : {"version", "module_id", "module_type", "set", "timestamp", "values"}) {
            auto payload = to_payload(livedata_envelope());
            payload.erase(field);
            const auto parsed = parse_envelope(topic, payload);
            CHECK(parsed.error == ParseError::MissingField);
            CHECK(parsed.message == field);
        }
    }

    SECTION("a required field of the wrong type names the field") {
        auto payload = to_payload(livedata_envelope());
        payload["values"] = "not an object";
        const auto parsed = parse_envelope(topic, payload);
        CHECK(parsed.error == ParseError::WrongFieldType);
        CHECK(parsed.message == "values");
    }

    SECTION("optional fields of the wrong type are refused too") {
        for (const auto* field : {"instance", "mapping", "seq", "labels"}) {
            auto payload = to_payload(livedata_envelope());
            payload[field] = 0.5;
            const auto parsed = parse_envelope(topic, payload);
            CHECK(parsed.error == ParseError::WrongFieldType);
            CHECK(parsed.message == field);
        }
    }

    SECTION("a payload that disagrees with its topic is refused") {
        const auto payload = to_payload(livedata_envelope());
        CHECK(parse_envelope("everest-telemetry/v1/powermeter_2/livedata", payload).error ==
              ParseError::TopicMismatch);
        CHECK(parse_envelope("everest-telemetry/v1/powermeter_1/diagnostics", payload).error ==
              ParseError::TopicMismatch);
    }

    SECTION("a topic outside the value flow is refused before the payload is looked at") {
        CHECK(parse_envelope("everest/powermeter_1/var", nlohmann::json::object()).error ==
              ParseError::NotATelemetryTopic);
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

TEST_CASE("an unset filter member matches anything", "[telemetry][filter]") {
    auto envelope = livedata_envelope();
    envelope.instance = "converter_7";

    CHECK(Filter{}.matches(envelope));

    SECTION("module id") {
        Filter filter;
        filter.module_id = "powermeter_1";
        CHECK(filter.matches(envelope));
        filter.module_id = "powermeter_2";
        CHECK_FALSE(filter.matches(envelope));
    }

    SECTION("module type, so a consumer can filter without knowing instance names") {
        Filter filter;
        filter.module_type = "LemDCBM400600";
        CHECK(filter.matches(envelope));
        filter.module_type = "Huawei_V100R023C10";
        CHECK_FALSE(filter.matches(envelope));
    }

    SECTION("set") {
        Filter filter;
        filter.set = "livedata";
        CHECK(filter.matches(envelope));
        filter.set = "diagnostics";
        CHECK_FALSE(filter.matches(envelope));
    }

    SECTION("instance") {
        Filter filter;
        filter.instance = "converter_7";
        CHECK(filter.matches(envelope));
        filter.instance = "converter_8";
        CHECK_FALSE(filter.matches(envelope));

        SECTION("an instance filter does not match an envelope without an instance") {
            const auto unaddressed = livedata_envelope();
            filter.instance = "converter_7";
            CHECK_FALSE(filter.matches(unaddressed));
        }
    }

    SECTION("evse and connector require a mapping") {
        Filter filter;
        filter.evse = 1;
        CHECK(filter.matches(envelope));
        filter.evse = 2;
        CHECK_FALSE(filter.matches(envelope));

        filter = Filter{};
        filter.connector = 1;
        CHECK_FALSE(filter.matches(envelope)); // mapping has no connector

        envelope.mapping = Mapping{1, 1};
        CHECK(filter.matches(envelope));

        envelope.mapping.reset();
        filter = Filter{};
        filter.evse = 1;
        CHECK_FALSE(filter.matches(envelope));
    }

    SECTION("members combine as a conjunction") {
        Filter filter;
        filter.module_type = "LemDCBM400600";
        filter.set = "diagnostics";
        CHECK_FALSE(filter.matches(envelope));
    }
}

TEST_CASE("values are validated against their declaration", "[telemetry][validation]") {
    const auto definitions = powermeter_definitions();
    auto envelope = livedata_envelope();

    SECTION("declared values pass") {
        envelope.values = Values{{"temperature_C", 41.2}, {"fw_state", std::string{"Measuring"}}};
        CHECK(validate(envelope, definitions).ok());
    }

    SECTION("an integral value satisfies a number declaration") {
        envelope.values = Values{{"temperature_C", std::int64_t{41}}};
        CHECK(validate(envelope, definitions).ok());
    }

    SECTION("an undeclared entry is a finding on that entry") {
        envelope.values = Values{{"temperature_C", 41.2}, {"undeclared", 1.0}};
        const auto result = validate(envelope, definitions);
        REQUIRE(result.findings.size() == 1);
        CHECK(result.findings.front().entry == "undeclared");
        CHECK(result.findings.front().issue == ValueIssue::UndeclaredEntry);
    }

    SECTION("a wrong type is a finding") {
        envelope.values = Values{{"temperature_C", std::string{"warm"}}};
        const auto result = validate(envelope, definitions);
        REQUIRE(result.findings.size() == 1);
        CHECK(result.findings.front().issue == ValueIssue::TypeMismatch);
    }

    SECTION("declared limits are enforced") {
        envelope.values = Values{{"temperature_C", 150.0}};
        CHECK(validate(envelope, definitions).findings.front().issue == ValueIssue::OutOfRange);

        envelope.values = Values{{"temperature_C", -50.0}};
        CHECK(validate(envelope, definitions).findings.front().issue == ValueIssue::OutOfRange);

        envelope.values = Values{{"frequency_Hz", 5000.0}};
        CHECK(validate(envelope, definitions).ok()); // no limits declared
    }

    SECTION("a values list is enforced") {
        envelope.values = Values{{"fw_state", std::string{"Rebooting"}}};
        CHECK(validate(envelope, definitions).findings.front().issue == ValueIssue::NotInValuesList);
    }

    SECTION("an undeclared set or module invalidates the whole envelope") {
        envelope.set = "diagnostics";
        auto result = validate(envelope, definitions);
        REQUIRE(result.findings.size() == 1);
        CHECK(result.findings.front().entry.empty());
        CHECK(result.findings.front().issue == ValueIssue::UndeclaredSet);

        envelope = livedata_envelope();
        envelope.module_id = "powermeter_2";
        result = validate(envelope, definitions);
        REQUIRE(result.findings.size() == 1);
        CHECK(result.findings.front().issue == ValueIssue::UndeclaredModule);
    }

    SECTION("every offending value is reported, not just the first") {
        envelope.values = Values{{"temperature_C", std::string{"warm"}}, {"undeclared", 1.0}};
        CHECK(validate(envelope, definitions).findings.size() == 2);
    }
}
