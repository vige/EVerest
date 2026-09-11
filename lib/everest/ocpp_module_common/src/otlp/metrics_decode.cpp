// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/ocpp_module_common/otlp/metrics_decode.hpp>

#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include <opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h>

namespace ocpp_module_common::otlp {

namespace {

namespace proto = opentelemetry::proto;

using Attributes = std::map<std::string, std::string>;

/// \brief A double as short text, so an attribute written as 1 does not read back as "1.000000".
std::string to_text(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic()); // a decimal comma would be a different attribute value
    out.precision(17);
    out << value;
    return out.str();
}

/// \brief An AnyValue as text. Arrays, maps and byte strings have no text form worth inventing, so
/// they are dropped rather than guessed at.
///
/// An if-chain rather than a switch: the library is built with -Werror=switch-enum, and the point
/// of this function is that the cases it does not name are all handled the same way.
std::optional<std::string> to_text(const proto::common::v1::AnyValue& value) {
    using AnyValue = proto::common::v1::AnyValue;
    const auto which = value.value_case();
    if (which == AnyValue::kStringValue) {
        return value.string_value();
    }
    if (which == AnyValue::kBoolValue) {
        return value.bool_value() ? "true" : "false";
    }
    if (which == AnyValue::kIntValue) {
        return std::to_string(value.int_value());
    }
    if (which == AnyValue::kDoubleValue) {
        return to_text(value.double_value());
    }
    return std::nullopt;
}

/// \brief Adds the readable attributes of \p from to \p into, overwriting what is already there.
///
/// The caller decides the order, and it matters: a data point attribute is the more specific
/// statement, so it is added after the resource and wins.
void add_attributes(const google::protobuf::RepeatedPtrField<proto::common::v1::KeyValue>& from, Attributes& into) {
    for (const auto& attribute : from) {
        if (attribute.key().empty()) {
            continue;
        }
        if (const auto text = to_text(attribute.value()); text.has_value()) {
            into[attribute.key()] = *text;
        }
    }
}

/// \brief Turns the data points of a Gauge or a Sum into samples.
void read_points(const google::protobuf::RepeatedPtrField<proto::metrics::v1::NumberDataPoint>& points,
                 const Attributes& inherited, const Sample& prototype, Export& out) {
    for (const auto& point : points) {
        Sample sample = prototype;
        sample.attributes = inherited;
        add_attributes(point.attributes(), sample.attributes);
        sample.time_unix_nano = point.time_unix_nano();

        // a point that carries neither reads as a double zero, which is what the value oneof
        // defaults to and the only reading available
        if (point.has_as_int()) {
            sample.is_integer = true;
            sample.as_int = point.as_int();
        } else {
            sample.is_integer = false;
            sample.as_double = point.as_double();
        }
        out.samples.push_back(std::move(sample));
    }
}

} // namespace

std::optional<Export> decode_export_metrics_request(std::string_view body) {
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    proto::collector::metrics::v1::ExportMetricsServiceRequest request;
    if (not request.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        return std::nullopt;
    }

    Export out;
    for (const auto& resource_metrics : request.resource_metrics()) {
        Attributes resource_attributes;
        if (resource_metrics.has_resource()) {
            add_attributes(resource_metrics.resource().attributes(), resource_attributes);
        }

        for (const auto& scope_metrics : resource_metrics.scope_metrics()) {
            for (const auto& metric : scope_metrics.metrics()) {
                Sample prototype;
                prototype.metric = metric.name();
                prototype.unit = metric.unit();
                prototype.description = metric.description();

                if (metric.has_gauge() or metric.has_sum()) {
                    const auto& points =
                        metric.has_gauge() ? metric.gauge().data_points() : metric.sum().data_points();
                    // a metric with no name cannot be mapped onto anything, and an anonymous sample
                    // in front of the matcher is worse than one that was counted and dropped
                    if (prototype.metric.empty()) {
                        out.skipped_points += static_cast<std::size_t>(points.size());
                    } else {
                        read_points(points, resource_attributes, prototype, out);
                    }
                }

                // the shapes this receiver has no device model reading for: counted, so the caller
                // can report how much of the request went unused without calling it an error
                if (metric.has_histogram()) {
                    out.skipped_points += static_cast<std::size_t>(metric.histogram().data_points_size());
                }
                if (metric.has_exponential_histogram()) {
                    out.skipped_points += static_cast<std::size_t>(metric.exponential_histogram().data_points_size());
                }
                if (metric.has_summary()) {
                    out.skipped_points += static_cast<std::size_t>(metric.summary().data_points_size());
                }
            }
        }
    }
    return out;
}

} // namespace ocpp_module_common::otlp
