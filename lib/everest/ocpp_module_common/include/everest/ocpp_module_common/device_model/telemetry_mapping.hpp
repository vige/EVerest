// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <everest/telemetry/sink.hpp>
#include <ocpp/v2/ocpp_types.hpp>

/// \file
/// The telemetry mapping file: its schema, its parser and the validated list a parse produces.
///
/// Telemetry sets are free to grow. The CSMS-facing device model is not, so the two are joined by a
/// curated list rather than by a rule: an entry becomes a device model variable when, and only when,
/// this file says so. The file names the OCPP side (component, variable) and the EVerest side
/// (module instance, set, entry); everything else about the variable -- its type, unit and bounds --
/// comes from the set declaration in the producer's manifest, so a mapping cannot describe a value
/// differently from the module that publishes it.
///
/// Nothing here reads a value or talks to the framework, so it is testable on its own.
namespace ocpp_module_common::device_model {

/// \brief One validated mapping: an OCPP target and the single telemetry entry it reads from.
struct TelemetryMapping {
    ocpp::v2::Component component;
    ocpp::v2::Variable variable;
    Everest::telemetry::SetKey flow; ///< publishing module instance and set
    std::string entry;               ///< entry name as declared by that set

    /// \returns "Component(instance,evse)/Variable <- module/set.entry", for logs
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
///   everest:
///     module_id: powermeter_1
///     telemetry_set: livedata
///     entry: temperature_C
/// \endcode
/// where `instance`, `evse` and `connector` are optional. A missing file is an error, not an empty
/// list: a station configured with a mapping path that does not resolve is misconfigured, and
/// silently serving nothing would hide it.
TelemetryMappingLoad load_telemetry_mappings(const std::filesystem::path& path);

} // namespace ocpp_module_common::device_model
