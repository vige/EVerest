// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// \file
/// \brief Reading an OTLP metrics export into something a device model can use.
///
/// The wire message is a tree -- resource, scope, metric, data point -- and every level carries
/// attributes that the level below inherits. A device model has no use for the tree: it wants a
/// flat list of "this named measurement, with these attributes, reads this". Flattening happens
/// here, once, so nothing downstream has to know what a ScopeMetrics is.
///
/// \par The schema this implements
/// opentelemetry-proto, as vendored by opentelemetry-cpp and compiled by protoc during the build:
/// this reads `ExportMetricsServiceRequest` through the generated classes, so the receiver and the
/// producers of the same station are decoding and encoding the same definitions. Both halves are
/// built only with EVEREST_ENABLE_OTLP_TELEMETRY, and both get protobuf from the same place.
///
/// What is left here is the flattening and the policy: which shapes have a device model reading
/// (Gauge and Sum), which are counted and dropped (Histogram, ExponentialHistogram, Summary), and
/// how an attribute value becomes the text a mapping file can be written against.
namespace ocpp_module_common::otlp {

/// \brief One numeric data point, with everything the tree above it contributed.
struct Sample {
    std::string metric;      ///< Metric.name, e.g. "system.cpu.utilization"
    std::string unit;        ///< Metric.unit as UCUM, empty when the producer sent none
    std::string description; ///< Metric.description, empty when the producer sent none

    /// \brief Resource attributes and data point attributes, merged, values as text.
    ///
    /// A data point attribute wins over a resource attribute of the same name: the more specific
    /// statement about a measurement is the one nearer to it. Values are rendered as text because
    /// that is what a mapping file compares against and what a device model reports; a number
    /// keeps its own spelling, so an attribute written as 1 does not become "1.000000".
    std::map<std::string, std::string> attributes;

    std::uint64_t time_unix_nano{0}; ///< when the producer says it measured, 0 when unset

    /// \brief The value, and which of the two ways it arrived.
    ///
    /// OTLP numbers are a double or a signed 64-bit integer, and the difference survives here so
    /// that an integer variable is not reported to a CSMS with a decimal point.
    bool is_integer{false};
    double as_double{0.0};
    std::int64_t as_int{0};

    /// \returns the value as a double, whichever way it arrived
    double value() const {
        return is_integer ? static_cast<double>(as_int) : as_double;
    }
};

/// \brief What one export request contained.
struct Export {
    std::vector<Sample> samples;

    /// \brief Data points in a shape this decoder does not read: histograms, exponential
    /// histograms and summaries.
    ///
    /// Not an error. A producer is free to export a histogram, and a station that has no use for
    /// one should accept the request and ignore it rather than reject a batch that also carries
    /// gauges somebody mapped.
    std::size_t skipped_points{0};
};

/// \brief Decodes an ExportMetricsServiceRequest.
///
/// \param body the request body exactly as it arrived, not null terminated
/// \returns the samples it carried, or nothing when the body is not a well formed message
///
/// An empty body is a well formed request carrying nothing, which is what a producer with no
/// metrics to report sends. That returns an empty Export rather than a failure.
std::optional<Export> decode_export_metrics_request(std::string_view body);

} // namespace ocpp_module_common::otlp
