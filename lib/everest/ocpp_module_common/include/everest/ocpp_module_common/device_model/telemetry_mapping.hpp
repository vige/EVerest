// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <ocpp/v2/ocpp_types.hpp>

#include <everest/ocpp_module_common/otlp/metrics_decode.hpp>

/// \file
/// The telemetry mapping file: its schema, its parser and the validated list a parse produces.
///
/// A station measures far more than an operator wants a CSMS to see, and anything at all may push
/// a metric to the receiver. The two are joined by a curated list rather than by a rule: a metric
/// becomes a device model variable when, and only when, this file says so.
///
/// The file also carries the characteristics -- data type, unit, bounds, allowed values. OTLP has
/// no field for bounds or an enumeration, and a metric from a host daemon has no EVerest
/// declaration to read them from, so this is the only place they can live. That makes the file a
/// contract with the operator rather than a routing table, and it is what lets the device model be
/// complete before a single measurement has arrived.
///
/// Nothing here reads a value or talks to the framework, so it is testable on its own.
namespace ocpp_module_common::device_model {

/// \brief Which metric, and which of its data points.
///
/// A metric usually carries several data points that differ only by attribute -- one per EVSE, one
/// per phase. The selector names the metric and however many attributes it takes to pick one of
/// them out.
struct MetricSelector {
    std::string metric;                            ///< OTLP Metric.name, matched exactly
    std::map<std::string, std::string> attributes; ///< every one must be present and equal

    /// \returns true when \p sample is the measurement this selector names
    ///
    /// Attributes the sample carries and the selector does not name are ignored, so a selector can
    /// say "evse 1" without having to know what else the producer attaches to the point.
    bool matches(const otlp::Sample& sample) const;

    /// \returns "metric{key=value,...}", for logs
    std::string to_string() const;
};

/// \brief One validated mapping: an OCPP target, what it reads, and how it is described.
struct TelemetryMapping {
    ocpp::v2::Component component;
    ocpp::v2::Variable variable;
    MetricSelector selector;
    ocpp::v2::VariableCharacteristics characteristics;

    /// \returns "Component(instance,evse)/Variable <- metric{attributes}", for logs
    std::string to_string() const;
};

/// \brief What a parse produced, and what it threw away.
///
/// The rejections are carried rather than only logged so that a caller can report a count, and a
/// unit test can assert the rejection without capturing EVLOG.
struct TelemetryMappingLoad {
    std::vector<TelemetryMapping> mappings;
    std::vector<std::string> rejected; ///< one message per malformed entry
    std::vector<std::string> errors;   ///< one message per whole-file failure
};

/// \brief Parses the YAML mapping file at \p path.
///
/// The file is a `telemetry_mappings:` list of
/// \code{.yaml}
/// - ocpp:
///     component: { name: PowerMeterDC, instance: A, evse: { id: 1, connector: 1 } }
///     variable: { name: MeterTemperature, instance: Max }
///     characteristics: { dataType: decimal, unit: Celsius, min: -40, max: 120 }
///   otel:
///     metric: powermeter.temperature
///     attributes: { evse: 1 }
/// \endcode
/// where `instance`, `evse`, `connector`, `attributes` and every member of `characteristics`
/// except `dataType` are optional. Attribute values are compared as text, so `1` and `"1"` are the
/// same selector.
///
/// A missing file is an error, not an empty list: a station configured with a mapping path that
/// does not resolve is misconfigured, and silently serving nothing would hide it.
TelemetryMappingLoad load_telemetry_mappings(const std::filesystem::path& path);

} // namespace ocpp_module_common::device_model
