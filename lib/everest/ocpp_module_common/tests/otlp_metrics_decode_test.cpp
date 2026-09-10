// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <everest/ocpp_module_common/otlp/metrics_decode.hpp>
#include <everest/ocpp_module_common/otlp/wire.hpp>

/// \file
/// \brief Tests for the OTLP metrics decoder.
///
/// The payloads under data/otlp come from a real OpenTelemetry SDK, not from this test: a fixture
/// this file encoded itself would only show that the decoder agrees with its own idea of the wire
/// format. data/otlp/generate.py rebuilds them.

namespace {

using namespace ocpp_module_common::otlp;

std::string fixture(const std::string& name) {
    const std::string path = std::string(TEST_DATA_DIR) + "/otlp/" + name;
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << "missing fixture " << path;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const Sample* find(const Export& decoded, const std::string& metric, const std::string& attribute = "",
                   const std::string& value = "") {
    for (const auto& sample : decoded.samples) {
        if (sample.metric != metric) {
            continue;
        }
        if (attribute.empty()) {
            return &sample;
        }
        const auto found = sample.attributes.find(attribute);
        if (found != sample.attributes.end() and found->second == value) {
            return &sample;
        }
    }
    return nullptr;
}

TEST(OtlpMetricsDecode, DecodesADoubleGaugeWithItsNameUnitAndAttributes) {
    const auto decoded = decode_export_metrics_request(fixture("gauge_double.bin"));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->samples.size(), 1);

    const auto& sample = decoded->samples.front();
    EXPECT_EQ(sample.metric, "powermeter.temperature");
    EXPECT_EQ(sample.unit, "Cel");
    EXPECT_EQ(sample.description, "Board temperature");
    EXPECT_FALSE(sample.is_integer);
    EXPECT_DOUBLE_EQ(sample.as_double, 41.5);
    EXPECT_GT(sample.time_unix_nano, 0);

    // the data point attribute and the resource attributes arrive merged
    EXPECT_EQ(sample.attributes.at("evse"), "1");
    EXPECT_EQ(sample.attributes.at("service.name"), "powermeter_1");
    EXPECT_EQ(sample.attributes.at("station.serial"), "KP-0001");
}

TEST(OtlpMetricsDecode, KeepsIntegersIntegral) {
    const auto decoded = decode_export_metrics_request(fixture("mixed.bin"));
    ASSERT_TRUE(decoded.has_value());

    const auto* uptime = find(*decoded, "system.uptime");
    ASSERT_NE(uptime, nullptr);
    EXPECT_TRUE(uptime->is_integer);
    EXPECT_EQ(uptime->as_int, 86400);
    EXPECT_EQ(uptime->unit, "s");

    // a negative sfixed64 is two's complement, not a varint, and gets this wrong very visibly
    const auto* rsrp = find(*decoded, "modem.rsrp");
    ASSERT_NE(rsrp, nullptr);
    EXPECT_TRUE(rsrp->is_integer);
    EXPECT_EQ(rsrp->as_int, -97);
    EXPECT_TRUE(rsrp->unit.empty());
}

TEST(OtlpMetricsDecode, ReadsSumsAsWellAsGauges) {
    const auto decoded = decode_export_metrics_request(fixture("mixed.bin"));
    ASSERT_TRUE(decoded.has_value());

    const auto* energy = find(*decoded, "powermeter.energy_imported");
    ASSERT_NE(energy, nullptr);
    EXPECT_TRUE(energy->is_integer);
    EXPECT_EQ(energy->as_int, 12345);
    EXPECT_EQ(energy->unit, "W.h");
}

TEST(OtlpMetricsDecode, OnePointPerDataPointNotPerMetric) {
    const auto decoded = decode_export_metrics_request(fixture("mixed.bin"));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->samples.size(), 5); // two temperatures, uptime, energy, rsrp

    const auto* evse1 = find(*decoded, "powermeter.temperature", "evse", "1");
    const auto* evse2 = find(*decoded, "powermeter.temperature", "evse", "2");
    ASSERT_NE(evse1, nullptr);
    ASSERT_NE(evse2, nullptr);
    EXPECT_DOUBLE_EQ(evse1->as_double, 41.5);
    EXPECT_DOUBLE_EQ(evse2->as_double, 38.25);
}

TEST(OtlpMetricsDecode, RendersEveryAttributeValueTypeAsText) {
    const auto decoded = decode_export_metrics_request(fixture("attribute_types.bin"));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->samples.size(), 1);

    const auto& attributes = decoded->samples.front().attributes;
    EXPECT_EQ(attributes.at("text"), "measuring");
    EXPECT_EQ(attributes.at("flag"), "true");
    EXPECT_EQ(attributes.at("count"), "7");
    // a whole number keeps its short spelling, so a mapping written as 1 still matches
    EXPECT_EQ(attributes.at("ratio"), "0.25");
}

TEST(OtlpMetricsDecode, ADataPointAttributeWinsOverTheResource) {
    const auto decoded = decode_export_metrics_request(fixture("shadowed_attribute.bin"));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->samples.size(), 1);

    // the resource says evse=0, the data point says evse=1; the nearer statement is the true one
    EXPECT_EQ(decoded->samples.front().attributes.at("evse"), "1");
}

TEST(OtlpMetricsDecode, AnEmptyBodyIsAValidExportOfNothing) {
    const auto decoded = decode_export_metrics_request(fixture("empty.bin"));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->samples.empty());
    EXPECT_EQ(decoded->skipped_points, 0);
}

TEST(OtlpMetricsDecode, RejectsATruncatedBody) {
    const auto whole = fixture("mixed.bin");
    ASSERT_GT(whole.size(), 20);

    // every prefix that cuts a length-delimited field short has to be refused rather than half read
    std::size_t refused = 0;
    for (std::size_t length = 1; length < whole.size(); ++length) {
        if (not decode_export_metrics_request(std::string_view(whole).substr(0, length)).has_value()) {
            refused += 1;
        }
    }
    EXPECT_GT(refused, whole.size() / 2) << "a truncated request was read as if it were whole";
}

TEST(OtlpMetricsDecode, RejectsGarbage) {
    EXPECT_FALSE(decode_export_metrics_request(std::string("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 11))
                     .has_value());
    // field number zero does not exist
    EXPECT_FALSE(decode_export_metrics_request(std::string("\x00\x01", 2)).has_value());
    // a length that runs past the end of the buffer
    EXPECT_FALSE(decode_export_metrics_request(std::string("\x0a\x7f\x01", 3)).has_value());
}

TEST(OtlpMetricsDecode, IgnoresAFieldItDoesNotKnow) {
    // an unknown field at the top level, then the real request: a newer producer must not break us
    auto body = std::string("\x7a\x03qqq", 5) + fixture("gauge_double.bin");
    const auto decoded = decode_export_metrics_request(body);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->samples.size(), 1);
    EXPECT_EQ(decoded->samples.front().metric, "powermeter.temperature");
}

TEST(OtlpWire, SkipsEveryWireTypeByLength) {
    // varint, fixed64, length delimited, fixed32 -- one field of each, then a marker we must reach
    const std::string body("\x08\x96\x01"          // field 1, varint 150
                           "\x11\x00\x00\x00\x00\x00\x00\x00\x00" // field 2, fixed64
                           "\x1a\x02hi"            // field 3, "hi"
                           "\x25\x00\x00\x00\x00"  // field 4, fixed32
                           "\x28\x07",             // field 5, varint 7
                           3 + 9 + 4 + 5 + 2);
    Reader reader(body);
    std::uint32_t last = 0;
    std::uint64_t last_value = 0;
    while (const auto tag = reader.next()) {
        last = tag->field;
        if (tag->field == 5) {
            const auto value = reader.varint();
            ASSERT_TRUE(value.has_value());
            last_value = *value;
        } else {
            ASSERT_TRUE(reader.skip(tag->type));
        }
    }
    EXPECT_TRUE(reader.ok());
    EXPECT_EQ(last, 5);
    EXPECT_EQ(last_value, 7);
}

TEST(OtlpWire, RefusesAVarintThatNeverEnds) {
    const std::string body("\x08\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80", 13);
    Reader reader(body);
    const auto tag = reader.next();
    ASSERT_TRUE(tag.has_value());
    EXPECT_FALSE(reader.varint().has_value());
    EXPECT_FALSE(reader.ok());
}

} // namespace
