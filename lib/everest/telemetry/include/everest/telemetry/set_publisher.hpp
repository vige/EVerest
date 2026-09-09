// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <generated/types/telemetry.hpp>
#include <utils/date.hpp>

namespace Everest::telemetry {

/// \brief The producer-side behaviour of one telemetry set.
///
/// A publishing implementation owns one of these. Client code never sees the interest table: it
/// declares a set, offers samples, and this decides what reaches the bus.
///
/// Four things the design note asks of a publisher, and nothing else:
///
/// - **Lazy.** Nothing is published until a subscriber declares interest. `wanted()` lets the module
///   skip sampling entirely while the active set is empty.
/// - **Union.** Interest is per subscriber; what is published is the union over all of them. A
///   subscriber therefore receives at least what it asked for and possibly more.
/// - **De-duplicated.** An unchanged value is not re-published, so absence of a message means the
///   value has not changed. Exact equality, no dead band.
/// - **Snapshot on change.** A subscriber that declares interest gets the cached value of everything
///   it asked for, so it does not have to wait for the next change. This is also what makes the
///   cache complete for late subscribers.
///
/// A producer-side rate cap coalesces updates to `max_publish_rate_hz`.
class SetPublisher {
public:
    using Publish = std::function<void(const types::telemetry::Update&)>;

    SetPublisher(types::telemetry::SetDefinition definition, Publish publish) :
        m_definition(std::move(definition)), m_publish(std::move(publish)) {
    }

    /// \brief Names the publishing module instance, for the attribution the payload carries.
    ///
    /// An implementation is constructed before the framework hands the module its info, so this
    /// arrives later, from the module loader.
    void set_module_id(std::string module_id) {
        m_module_id = std::move(module_id);
    }

    void set_mapping(std::optional<types::telemetry::Mapping> mapping) {
        m_mapping = std::move(mapping);
    }

    const types::telemetry::SetDefinition& definition() const {
        return m_definition;
    }

    /// \brief Records the interest of \p subscriber and publishes a snapshot of what it asked for.
    ///
    /// An empty \p entries withdraws. Entries the set does not declare are ignored.
    void set_interest(const std::string& subscriber, const std::vector<std::string>& entries) {
        std::vector<std::string> snapshot_of;
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            if (entries.empty()) {
                m_interest.erase(subscriber);
            } else {
                std::set<std::string> declared;
                for (const auto& entry : entries) {
                    if (declares(entry)) {
                        declared.insert(entry);
                    }
                }
                if (declared.empty()) {
                    m_interest.erase(subscriber);
                } else {
                    snapshot_of.assign(declared.begin(), declared.end());
                    m_interest[subscriber] = std::move(declared);
                }
            }
        }

        if (not snapshot_of.empty()) {
            publish_snapshot(snapshot_of);
        }
    }

    /// \returns the entries at least one subscriber wants
    std::set<std::string> wanted() const {
        std::lock_guard<std::mutex> lock{m_mutex};
        std::set<std::string> active;
        for (const auto& [subscriber, entries] : m_interest) {
            active.insert(entries.begin(), entries.end());
        }
        return active;
    }

    /// \brief Offers \p sample to the set. What is published is the wanted, changed subset of it,
    /// and nothing at all while the rate cap has not elapsed or nothing has changed.
    /// \returns true when an update was published
    bool offer(const json::object_t& sample) {
        json::object_t changed;
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            const auto now = std::chrono::steady_clock::now();
            if (m_last_publish.has_value() and m_definition.max_publish_rate_hz.has_value() and
                *m_definition.max_publish_rate_hz > 0) {
                const auto period = std::chrono::milliseconds(
                    static_cast<int>(1000.0 / static_cast<double>(*m_definition.max_publish_rate_hz)));
                if (now - *m_last_publish < period) {
                    return false;
                }
            }

            for (const auto& [entry, value] : sample) {
                if (not declares(entry)) {
                    continue;
                }
                // The cache holds every declared value, whether or not anybody wants it, so a later
                // subscriber gets a real snapshot rather than a hole.
                const auto cached = m_cache.find(entry);
                const bool unchanged = cached != m_cache.end() and cached->second == value;
                m_cache[entry] = value;
                if (not unchanged and is_wanted(entry)) {
                    changed[entry] = value;
                }
            }

            if (changed.empty()) {
                return false;
            }
            m_last_publish = now;
        }

        m_publish(update_of(std::move(changed)));
        return true;
    }

private:
    bool declares(const std::string& entry) const {
        for (const auto& declared : m_definition.entries) {
            if (declared.name == entry) {
                return true;
            }
        }
        return false;
    }

    /// \pre m_mutex is held
    bool is_wanted(const std::string& entry) const {
        for (const auto& [subscriber, entries] : m_interest) {
            if (entries.count(entry) > 0) {
                return true;
            }
        }
        return false;
    }

    types::telemetry::Update update_of(json::object_t values) const {
        types::telemetry::Update update;
        update.module_id = m_module_id;
        update.module_type = m_definition.module_type;
        update.set = m_definition.set;
        update.timestamp = ::Everest::Date::to_rfc3339(date::utc_clock::now());
        update.mapping = m_mapping;
        update.values = std::move(values);
        return update;
    }

    void publish_snapshot(const std::vector<std::string>& entries) {
        json::object_t values;
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            for (const auto& entry : entries) {
                const auto cached = m_cache.find(entry);
                if (cached != m_cache.end()) {
                    values[entry] = cached->second;
                }
            }
        }
        if (not values.empty()) {
            m_publish(update_of(std::move(values)));
        }
    }

    const types::telemetry::SetDefinition m_definition;
    std::string m_module_id;
    const Publish m_publish;

    mutable std::mutex m_mutex;
    std::optional<types::telemetry::Mapping> m_mapping;
    std::map<std::string, std::set<std::string>> m_interest; ///< subscriber module id -> its entries
    json::object_t m_cache;                                  ///< last value of every declared entry
    std::optional<std::chrono::steady_clock::time_point> m_last_publish;
};

/// \returns one declared entry
inline types::telemetry::EntryDefinition entry_of(const std::string& name, const std::string& description,
                                                  types::telemetry::EntryType type,
                                                  const std::optional<std::string>& unit = std::nullopt,
                                                  const std::optional<float>& minimum = std::nullopt,
                                                  const std::optional<float>& maximum = std::nullopt) {
    types::telemetry::EntryDefinition entry;
    entry.name = name;
    entry.description = description;
    entry.type = type;
    entry.unit = unit;
    entry.minimum = minimum;
    entry.maximum = maximum;
    return entry;
}

} // namespace Everest::telemetry
