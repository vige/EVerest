// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/ocpp_module_common/otlp/metrics_decode.hpp>

#include <cstring>
#include <locale>
#include <sstream>

#include <everest/ocpp_module_common/otlp/wire.hpp>

namespace ocpp_module_common::otlp {

namespace {

/// Field numbers from opentelemetry/proto/{common,resource,metrics}/v1, opentelemetry-proto 1.8.0.
/// Named rather than written at the point of use, because a wrong number here decodes silently into
/// the wrong thing. The schema is not vendored -- see the header for why, and for how to check
/// these against it.
namespace field {
// ExportMetricsServiceRequest
constexpr std::uint32_t REQUEST_RESOURCE_METRICS = 1;
// ResourceMetrics
constexpr std::uint32_t RESOURCE_METRICS_RESOURCE = 1;
constexpr std::uint32_t RESOURCE_METRICS_SCOPE_METRICS = 2;
// Resource
constexpr std::uint32_t RESOURCE_ATTRIBUTES = 1;
// ScopeMetrics
constexpr std::uint32_t SCOPE_METRICS_METRICS = 2;
// Metric
constexpr std::uint32_t METRIC_NAME = 1;
constexpr std::uint32_t METRIC_DESCRIPTION = 2;
constexpr std::uint32_t METRIC_UNIT = 3;
constexpr std::uint32_t METRIC_GAUGE = 5;
constexpr std::uint32_t METRIC_SUM = 7;
constexpr std::uint32_t METRIC_HISTOGRAM = 9;
constexpr std::uint32_t METRIC_EXPONENTIAL_HISTOGRAM = 10;
constexpr std::uint32_t METRIC_SUMMARY = 11;
// Gauge, Sum, Histogram, ... all carry their points in field 1
constexpr std::uint32_t POINTS_DATA_POINTS = 1;
// NumberDataPoint
constexpr std::uint32_t POINT_TIME_UNIX_NANO = 3;
constexpr std::uint32_t POINT_AS_DOUBLE = 4;
constexpr std::uint32_t POINT_AS_INT = 6;
constexpr std::uint32_t POINT_ATTRIBUTES = 7;
// KeyValue
constexpr std::uint32_t KEY_VALUE_KEY = 1;
constexpr std::uint32_t KEY_VALUE_VALUE = 2;
// AnyValue
constexpr std::uint32_t ANY_STRING = 1;
constexpr std::uint32_t ANY_BOOL = 2;
constexpr std::uint32_t ANY_INT = 3;
constexpr std::uint32_t ANY_DOUBLE = 4;
} // namespace field

using Attributes = std::map<std::string, std::string>;

/// \brief A double as short text, so an attribute written as 1 does not read back as "1.000000".
std::string to_text(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic()); // a decimal comma would be a different attribute value
    out.precision(17);
    out << value;
    return out.str();
}

/// \brief Reads an AnyValue into text. Arrays and nested maps have no text form worth inventing,
/// so they are dropped rather than guessed at.
std::optional<std::string> read_any_value(Reader& reader) {
    std::optional<std::string> text;
    while (const auto tag = reader.next()) {
        switch (tag->field) {
        case field::ANY_STRING: {
            const auto view = reader.bytes();
            if (not view.has_value()) {
                return std::nullopt;
            }
            text = std::string(*view);
            break;
        }
        case field::ANY_BOOL: {
            const auto value = reader.varint();
            if (not value.has_value()) {
                return std::nullopt;
            }
            text = (*value != 0) ? "true" : "false";
            break;
        }
        case field::ANY_INT: {
            const auto value = reader.varint();
            if (not value.has_value()) {
                return std::nullopt;
            }
            text = std::to_string(static_cast<std::int64_t>(*value));
            break;
        }
        case field::ANY_DOUBLE: {
            const auto value = reader.real();
            if (not value.has_value()) {
                return std::nullopt;
            }
            text = to_text(*value);
            break;
        }
        default:
            if (not reader.skip(tag->type)) {
                return std::nullopt;
            }
            break;
        }
    }
    return reader.ok() ? text : std::nullopt;
}

/// \brief Reads one KeyValue into \p into. A key with no readable value is left out.
bool read_key_value(Reader& reader, Attributes& into) {
    std::string key;
    std::optional<std::string> value;
    while (const auto tag = reader.next()) {
        switch (tag->field) {
        case field::KEY_VALUE_KEY: {
            const auto view = reader.bytes();
            if (not view.has_value()) {
                return false;
            }
            key = std::string(*view);
            break;
        }
        case field::KEY_VALUE_VALUE: {
            auto value_reader = reader.message();
            if (not value_reader.has_value()) {
                return false;
            }
            value = read_any_value(*value_reader);
            break;
        }
        default:
            if (not reader.skip(tag->type)) {
                return false;
            }
            break;
        }
    }
    if (not reader.ok()) {
        return false;
    }
    if (not key.empty() and value.has_value()) {
        into[key] = *value;
    }
    return true;
}

/// \brief Reads one NumberDataPoint onto the end of \p out.
bool read_number_data_point(Reader& reader, const Attributes& inherited, const Sample& prototype, Export& out) {
    Sample sample = prototype;
    sample.attributes = inherited;

    while (const auto tag = reader.next()) {
        switch (tag->field) {
        case field::POINT_TIME_UNIX_NANO: {
            const auto value = reader.fixed64();
            if (not value.has_value()) {
                return false;
            }
            sample.time_unix_nano = *value;
            break;
        }
        case field::POINT_AS_DOUBLE: {
            const auto value = reader.real();
            if (not value.has_value()) {
                return false;
            }
            sample.as_double = *value;
            sample.is_integer = false;
            break;
        }
        case field::POINT_AS_INT: {
            // sfixed64: two's complement in the low eight bytes, not a varint
            const auto bits = reader.fixed64();
            if (not bits.has_value()) {
                return false;
            }
            std::int64_t value = 0;
            std::memcpy(&value, &*bits, sizeof(value));
            sample.as_int = value;
            sample.is_integer = true;
            break;
        }
        case field::POINT_ATTRIBUTES: {
            auto attribute = reader.message();
            // a data point attribute is the more specific statement, so it wins over the resource
            if (not attribute.has_value() or not read_key_value(*attribute, sample.attributes)) {
                return false;
            }
            break;
        }
        default:
            if (not reader.skip(tag->type)) {
                return false;
            }
            break;
        }
    }
    if (not reader.ok()) {
        return false;
    }
    out.samples.push_back(std::move(sample));
    return true;
}

/// \brief Reads the data points of a Gauge or a Sum. Both hold them in field 1.
bool read_number_points(Reader& reader, const Attributes& inherited, const Sample& prototype, Export& out) {
    while (const auto tag = reader.next()) {
        if (tag->field == field::POINTS_DATA_POINTS and tag->type == WireType::LengthDelimited) {
            auto point = reader.message();
            if (not point.has_value() or not read_number_data_point(*point, inherited, prototype, out)) {
                return false;
            }
        } else if (not reader.skip(tag->type)) {
            return false;
        }
    }
    return reader.ok();
}

/// \brief Counts the points of a shape this decoder does not read, so the caller can report how
/// much of the request went unused without pretending it was an error.
bool count_points(Reader& reader, std::size_t& into) {
    while (const auto tag = reader.next()) {
        if (tag->field == field::POINTS_DATA_POINTS and tag->type == WireType::LengthDelimited) {
            if (not reader.bytes().has_value()) {
                return false;
            }
            into += 1;
        } else if (not reader.skip(tag->type)) {
            return false;
        }
    }
    return reader.ok();
}

/// \brief Reads one Metric.
///
/// Field order is not guaranteed by protobuf, and a name arriving after its data points would
/// leave every sample of that metric unnamed. So the points are set aside as raw views on the
/// first pass and read on the second, once the name, unit and description are known.
bool read_metric(Reader& reader, const Attributes& inherited, Export& out) {
    Sample prototype;
    std::vector<std::string_view> number_points;
    std::vector<std::string_view> other_points;

    while (const auto tag = reader.next()) {
        switch (tag->field) {
        case field::METRIC_NAME:
        case field::METRIC_DESCRIPTION:
        case field::METRIC_UNIT: {
            const auto view = reader.bytes();
            if (not view.has_value()) {
                return false;
            }
            if (tag->field == field::METRIC_NAME) {
                prototype.metric = std::string(*view);
            } else if (tag->field == field::METRIC_UNIT) {
                prototype.unit = std::string(*view);
            } else {
                prototype.description = std::string(*view);
            }
            break;
        }
        case field::METRIC_GAUGE:
        case field::METRIC_SUM: {
            const auto view = reader.bytes();
            if (not view.has_value()) {
                return false;
            }
            number_points.push_back(*view);
            break;
        }
        case field::METRIC_HISTOGRAM:
        case field::METRIC_EXPONENTIAL_HISTOGRAM:
        case field::METRIC_SUMMARY: {
            const auto view = reader.bytes();
            if (not view.has_value()) {
                return false;
            }
            other_points.push_back(*view);
            break;
        }
        default:
            if (not reader.skip(tag->type)) {
                return false;
            }
            break;
        }
    }
    if (not reader.ok()) {
        return false;
    }

    // a metric with no name cannot be mapped onto anything, and reporting it as a sample would put
    // an anonymous entry in front of the matcher
    if (prototype.metric.empty()) {
        for (const auto& view : number_points) {
            Reader counter(view);
            if (not count_points(counter, out.skipped_points)) {
                return false;
            }
        }
    } else {
        for (const auto& view : number_points) {
            Reader points(view);
            if (not read_number_points(points, inherited, prototype, out)) {
                return false;
            }
        }
    }

    for (const auto& view : other_points) {
        Reader counter(view);
        if (not count_points(counter, out.skipped_points)) {
            return false;
        }
    }
    return true;
}

bool read_scope_metrics(Reader& reader, const Attributes& inherited, Export& out) {
    while (const auto tag = reader.next()) {
        if (tag->field == field::SCOPE_METRICS_METRICS and tag->type == WireType::LengthDelimited) {
            auto metric = reader.message();
            if (not metric.has_value() or not read_metric(*metric, inherited, out)) {
                return false;
            }
        } else if (not reader.skip(tag->type)) {
            return false;
        }
    }
    return reader.ok();
}

bool read_resource(Reader& reader, Attributes& into) {
    while (const auto tag = reader.next()) {
        if (tag->field == field::RESOURCE_ATTRIBUTES and tag->type == WireType::LengthDelimited) {
            auto attribute = reader.message();
            if (not attribute.has_value() or not read_key_value(*attribute, into)) {
                return false;
            }
        } else if (not reader.skip(tag->type)) {
            return false;
        }
    }
    return reader.ok();
}

/// \brief Reads one ResourceMetrics. Same two-pass reason as read_metric: the resource may follow
/// the scopes it applies to.
bool read_resource_metrics(Reader& reader, Export& out) {
    Attributes resource_attributes;
    std::vector<std::string_view> scopes;

    while (const auto tag = reader.next()) {
        switch (tag->field) {
        case field::RESOURCE_METRICS_RESOURCE: {
            auto resource = reader.message();
            if (not resource.has_value() or not read_resource(*resource, resource_attributes)) {
                return false;
            }
            break;
        }
        case field::RESOURCE_METRICS_SCOPE_METRICS: {
            const auto view = reader.bytes();
            if (not view.has_value()) {
                return false;
            }
            scopes.push_back(*view);
            break;
        }
        default:
            if (not reader.skip(tag->type)) {
                return false;
            }
            break;
        }
    }
    if (not reader.ok()) {
        return false;
    }

    for (const auto& view : scopes) {
        Reader scope(view);
        if (not read_scope_metrics(scope, resource_attributes, out)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<Export> decode_export_metrics_request(std::string_view body) {
    Reader reader(body);
    Export out;

    while (const auto tag = reader.next()) {
        if (tag->field == field::REQUEST_RESOURCE_METRICS and tag->type == WireType::LengthDelimited) {
            auto resource_metrics = reader.message();
            if (not resource_metrics.has_value() or not read_resource_metrics(*resource_metrics, out)) {
                return std::nullopt;
            }
        } else if (not reader.skip(tag->type)) {
            return std::nullopt;
        }
    }

    if (not reader.ok()) {
        return std::nullopt;
    }
    return out;
}

} // namespace ocpp_module_common::otlp
