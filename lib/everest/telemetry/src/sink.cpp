// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/telemetry/sink.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

#include <everest/logging.hpp>

namespace Everest::telemetry {

bool SetKey::operator<(const SetKey& other) const {
    return std::tie(module_id, set) < std::tie(other.module_id, other.set);
}

bool SetKey::operator==(const SetKey& other) const {
    return std::tie(module_id, set) == std::tie(other.module_id, other.set);
}

std::string SetKey::to_string() const {
    return module_id + "/" + set;
}

bool Filter::matches(const types::telemetry::SetDefinition& definition, const std::string& publisher_id,
                     const std::optional<::Mapping>& mapping) const {
    if (module_id.has_value() and *module_id != publisher_id) {
        return false;
    }
    if (module_type.has_value() and *module_type != definition.module_type) {
        return false;
    }
    if (set.has_value() and *set != definition.set) {
        return false;
    }
    if (evse.has_value() and (not mapping.has_value() or mapping->evse != *evse)) {
        return false;
    }
    return not wanted_entries(definition).empty();
}

std::vector<std::string> Filter::wanted_entries(const types::telemetry::SetDefinition& definition) const {
    std::vector<std::string> wanted;
    for (const auto& entry : definition.entries) {
        if (entries.empty() or std::find(entries.begin(), entries.end(), entry.name) != entries.end()) {
            wanted.push_back(entry.name);
        }
    }
    return wanted;
}

Sink::Sink(const Slots& slots, std::string consumer_id) :
    m_slots(slots),
    m_consumer_id(std::move(consumer_id)),
    m_definitions(slots.size()),
    m_interest(slots.size()) {
}

void Sink::subscribe(UpdateHandler handler) {
    m_handler = std::move(handler);
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        m_slots.at(index)->subscribe_update(
            [this, index](const types::telemetry::Update& update) { this->on_update(index, update); });
    }
}

std::size_t Sink::resolve_definitions() {
    std::size_t resolved = 0;
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        try {
            auto definition = m_slots.at(index)->call_get_definition();
            const SetKey key{m_slots.at(index)->module_id, definition.set};
            m_definitions_by_key.insert_or_assign(key, definition);
            m_definitions.at(index) = std::move(definition);
            ++resolved;
        } catch (const std::exception& e) {
            EVLOG_warning << "telemetry: slot " << index << " (" << m_slots.at(index)->module_id
                          << ") did not answer get_definition: " << e.what();
        }
    }
    return resolved;
}

std::size_t Sink::declare_interest(const Filter& filter) {
    std::size_t interested = 0;
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        const auto& definition = m_definitions.at(index);
        std::vector<std::string> wanted;
        if (definition.has_value() and
            filter.matches(*definition, m_slots.at(index)->module_id, m_slots.at(index)->get_mapping())) {
            wanted = filter.wanted_entries(*definition);
        }

        // An unchanged empty interest is not re-sent: set_interest with an empty list is a
        // withdrawal, and a withdrawal of nothing is noise on the bus.
        if (wanted.empty() and m_interest.at(index).empty()) {
            continue;
        }

        m_interest.at(index) = std::set<std::string>(wanted.begin(), wanted.end());
        Array entries;
        for (const auto& entry : wanted) {
            entries.push_back(entry);
        }
        m_slots.at(index)->call_set_interest(m_consumer_id, entries);
        if (not wanted.empty()) {
            ++interested;
        }
    }
    return interested;
}

void Sink::withdraw() {
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        if (m_interest.at(index).empty()) {
            continue;
        }
        m_interest.at(index).clear();
        m_slots.at(index)->call_set_interest(m_consumer_id, Array{});
    }
}

const types::telemetry::SetDefinition* Sink::definition(std::size_t index) const {
    if (index >= m_definitions.size() or not m_definitions.at(index).has_value()) {
        return nullptr;
    }
    return &m_definitions.at(index).value();
}

const types::telemetry::SetDefinition* Sink::definition(const SetKey& key) const {
    const auto it = m_definitions_by_key.find(key);
    return it == m_definitions_by_key.end() ? nullptr : &it->second;
}

const std::map<SetKey, types::telemetry::SetDefinition>& Sink::definitions() const {
    return m_definitions_by_key;
}

json::object_t Sink::values(const SetKey& key) const {
    const auto it = m_values.find(key);
    return it == m_values.end() ? json::object_t{} : it->second;
}

std::optional<json> Sink::value(const SetKey& key, const std::string& entry) const {
    const auto flow = m_values.find(key);
    if (flow == m_values.end()) {
        return std::nullopt;
    }
    const auto value = flow->second.find(entry);
    if (value == flow->second.end()) {
        return std::nullopt;
    }
    return value->second;
}

const std::set<std::string>& Sink::interest(std::size_t index) const {
    return m_interest.at(index);
}

void Sink::on_update(std::size_t index, types::telemetry::Update update) {
    // The publisher emits the union over all of its subscribers, so an update can carry entries this
    // sink did not ask for. Prune them: a sink that forwards what it was not asked to forward
    // silently widens its own contract.
    const auto& wanted = m_interest.at(index);
    if (not wanted.empty()) {
        for (auto it = update.values.begin(); it != update.values.end();) {
            it = wanted.count(it->first) == 0 ? update.values.erase(it) : std::next(it);
        }
    }

    if (update.values.empty()) {
        return;
    }

    // "No message" means "unchanged", so the cache is the only place a full picture exists.
    auto& cache = m_values[SetKey{update.module_id, update.set}];
    for (const auto& [entry, value] : update.values) {
        cache[entry] = value;
    }

    if (m_handler) {
        m_handler(update);
    }
}

} // namespace Everest::telemetry
