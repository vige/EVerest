// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <generated/interfaces/telemetry/Interface.hpp>

/// \file
/// \brief Consumer side of the EVerest telemetry feature.
///
/// Telemetry travels as an ordinary interface variable: a module provides one implementation of the
/// `telemetry` interface per set, and a sink is wired to the sets it may consume through the
/// `connections` block of `config.yaml`. This class is convenience over that slot vector, not a
/// framework facility - it owns nothing the framework does not already own.
///
/// Three things it does that a slot cannot do on its own:
///
/// - **Interest.** Nothing is published until a sink calls `set_interest`. A `Filter` is translated
///   into the matching per-slot `set_interest` calls, which is what opens the valve at the
///   publisher.
/// - **Overshoot.** A publisher emits the union of what all of its subscribers asked for, so a sink
///   receives at least what it wanted and possibly more. Entries outside the filter are pruned here.
/// - **Absence.** De-duplication means "no message" is "unchanged", so the last value of every entry
///   is kept.
///
/// Order matters. Subscriptions belong in `init`, interest in `ready`: the snapshot a publisher sends
/// on an interest change is lost if the handler is not registered yet.
namespace Everest::telemetry {

/// \brief The requirement slots of a sink, as the generated module class hands them out.
using Slots = std::vector<std::unique_ptr<telemetryIntf>>;

/// \brief Identity of one telemetry flow: one set of one publishing module instance.
struct SetKey {
    std::string module_id;
    std::string set;

    bool operator<(const SetKey& other) const;
    bool operator==(const SetKey& other) const;

    /// \returns "module_id/set", for logs
    std::string to_string() const;
};

/// \brief What a sink wants out of the sets it is wired to.
///
/// Every unset member matches everything. Unlike a broker subscription this is not a topic pattern:
/// the candidate set is what `config.yaml` wired, and the filter picks from it.
struct Filter {
    std::optional<std::string> module_id;   ///< publishing module instance
    std::optional<std::string> module_type; ///< publishing module type, from the set definition
    std::optional<std::string> set;         ///< implementation id of the set
    std::optional<int> evse;                ///< EVSE the publishing implementation is mapped to
    std::vector<std::string> entries;       ///< wanted entries, empty means every declared entry

    /// \returns true when \p definition and \p mapping satisfy every member that is set
    bool matches(const types::telemetry::SetDefinition& definition, const std::string& module_id,
                 const std::optional<::Mapping>& mapping) const;

    /// \returns the entries of \p definition this filter wants, in declaration order
    std::vector<std::string> wanted_entries(const types::telemetry::SetDefinition& definition) const;
};

class Sink {
public:
    using UpdateHandler = std::function<void(const types::telemetry::Update&)>;

    /// \param slots the sink's telemetry requirement slots, which must outlive the Sink
    /// \param consumer_id the sink's own module id, which the publisher keys its interest table by
    Sink(const Slots& slots, std::string consumer_id);

    /// \brief Registers \p handler on the update variable of every slot. Call from `init`.
    ///
    /// Nothing arrives until interest is declared. A slot the sink has no use for should be filtered
    /// out at wiring time rather than here: an update delivered and dropped has already cost the
    /// broker and one JSON parse.
    void subscribe(UpdateHandler handler);

    /// \brief Calls `get_definition` on every slot and caches the result. Call from `ready`.
    ///
    /// Optional: declare_interest() does this itself the first time, because a filter cannot be
    /// evaluated without the definitions. Call it directly only to read the definitions before
    /// declaring an interest - to build a device model, for instance.
    ///
    /// A slot that does not answer is left without a definition and is skipped by every filter.
    /// \returns the number of slots that answered
    std::size_t resolve_definitions();

    /// \brief Declares interest in the entries \p filter selects, per matching slot.
    ///
    /// Replaces any interest declared before: slots that no longer match are withdrawn. Resolves
    /// the definitions first if that has not happened yet, because a filter is evaluated against
    /// them.
    /// \returns the number of slots left with a non-empty interest
    std::size_t declare_interest(const Filter& filter);

    /// \brief Withdraws interest from every slot that has any. Publishing stops when the last
    /// subscriber withdraws.
    void withdraw();

    /// \returns the definition of the set behind slot \p index, or nothing when it did not answer
    const types::telemetry::SetDefinition* definition(std::size_t index) const;

    /// \returns the definition of \p key, or nothing when no slot serves it
    const types::telemetry::SetDefinition* definition(const SetKey& key) const;

    /// \returns every definition that resolved, keyed by flow
    const std::map<SetKey, types::telemetry::SetDefinition>& definitions() const;

    /// \returns the last value seen for every entry of \p key, empty when nothing has arrived
    json::object_t values(const SetKey& key) const;

    /// \returns the last value seen for \p entry of \p key, or nothing when it has never arrived
    std::optional<json> value(const SetKey& key, const std::string& entry) const;

    /// \returns the entries currently wanted from slot \p index
    const std::set<std::string>& interest(std::size_t index) const;

private:
    void on_update(std::size_t index, types::telemetry::Update update);

    const Slots& m_slots;
    const std::string m_consumer_id;
    UpdateHandler m_handler;

    bool m_resolved{false};
    std::vector<std::optional<types::telemetry::SetDefinition>> m_definitions;
    std::map<SetKey, types::telemetry::SetDefinition> m_definitions_by_key;
    std::vector<std::set<std::string>> m_interest;
    std::map<SetKey, json::object_t> m_values;
};

} // namespace Everest::telemetry
