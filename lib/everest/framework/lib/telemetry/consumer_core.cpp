// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <utils/telemetry/consumer_core.hpp>

#include <utility>

namespace Everest::telemetry {

/// \brief The subscription table, held by shared_ptr so that a Subscription handle can outlive the
/// core it came from without dangling.
struct ConsumerCore::Subscription::Registry {
    std::mutex mutex;
    SubscriptionId next_id{1};
    std::map<SubscriptionId, ConsumerCore::Entry> entries;
};

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

ConsumerCore::Subscription::Subscription(std::weak_ptr<Registry> registry, SubscriptionId id) :
    m_registry(std::move(registry)), m_id(id) {
}

ConsumerCore::Subscription::Subscription(Subscription&& other) noexcept :
    m_registry(std::move(other.m_registry)), m_id(other.m_id) {
    other.m_registry.reset();
    other.m_id = 0;
}

ConsumerCore::Subscription& ConsumerCore::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        m_registry = std::move(other.m_registry);
        m_id = other.m_id;
        other.m_registry.reset();
        other.m_id = 0;
    }
    return *this;
}

ConsumerCore::Subscription::~Subscription() {
    reset();
}

void ConsumerCore::Subscription::reset() {
    if (m_id == 0) {
        return;
    }
    if (const auto registry = m_registry.lock()) {
        const std::lock_guard<std::mutex> lock{registry->mutex};
        registry->entries.erase(m_id);
    }
    m_registry.reset();
    m_id = 0;
}

bool ConsumerCore::Subscription::active() const {
    if (m_id == 0) {
        return false;
    }
    const auto registry = m_registry.lock();
    if (not registry) {
        return false;
    }
    const std::lock_guard<std::mutex> lock{registry->mutex};
    return registry->entries.count(m_id) != 0;
}

// ---------------------------------------------------------------------------
// ConsumerCore
// ---------------------------------------------------------------------------

ConsumerCore::ConsumerCore() : ConsumerCore(Options{}) {
}

ConsumerCore::ConsumerCore(Options options) :
    m_registry(std::make_shared<Subscription::Registry>()), m_options(options) {
}

ConsumerCore::~ConsumerCore() = default;

void ConsumerCore::set_definitions(TelemetryDefinitions definitions) {
    const std::lock_guard<std::mutex> lock{m_mutex};
    m_definitions = std::move(definitions);
}

const TelemetryDefinitions& ConsumerCore::definitions() const {
    // The catalog is fetched once at startup, before any producer publishes, so reads are not
    // serialized against set_definitions().
    return m_definitions;
}

ConsumerCore::Subscription ConsumerCore::subscribe(Filter filter, Callback callback) {
    const std::lock_guard<std::mutex> lock{m_registry->mutex};
    const auto id = m_registry->next_id++;
    m_registry->entries.emplace(id, Entry{id, std::move(filter), std::move(callback)});
    return Subscription{m_registry, id};
}

std::size_t ConsumerCore::subscription_count() const {
    const std::lock_guard<std::mutex> lock{m_registry->mutex};
    return m_registry->entries.size();
}

void ConsumerCore::set_parse_error_handler(ParseErrorHandler handler) {
    const std::lock_guard<std::mutex> lock{m_mutex};
    m_parse_error_handler = std::move(handler);
}

void ConsumerCore::set_validation_handler(ValidationHandler handler) {
    const std::lock_guard<std::mutex> lock{m_mutex};
    m_validation_handler = std::move(handler);
}

ConsumerCore::Statistics ConsumerCore::statistics() const {
    const std::lock_guard<std::mutex> lock{m_mutex};
    return m_statistics;
}

void ConsumerCore::handle_message(std::string_view topic, std::string_view payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) {
        ParseResult result;
        result.error = ParseError::NotJson;

        ParseErrorHandler handler;
        {
            const std::lock_guard<std::mutex> lock{m_mutex};
            ++m_statistics.messages_received;
            ++m_statistics.parse_errors;
            handler = m_parse_error_handler;
        }
        if (handler) {
            handler(topic, result);
        }
        return;
    }
    handle_message(topic, parsed);
}

void ConsumerCore::handle_message(std::string_view topic, const nlohmann::json& payload) {
    auto result = parse_envelope(topic, payload);

    ParseErrorHandler parse_error_handler;
    ValidationHandler validation_handler;
    ValidationResult validation;
    Envelope envelope;
    bool dispatchable = false;

    {
        const std::lock_guard<std::mutex> lock{m_mutex};
        ++m_statistics.messages_received;

        if (not result.ok()) {
            ++m_statistics.parse_errors;
            parse_error_handler = m_parse_error_handler;
        } else {
            envelope = std::move(*result.envelope);
            dispatchable = true;

            if (m_options.validate_against_catalog) {
                validation = validate(envelope, m_definitions);
                if (not validation.ok()) {
                    ++m_statistics.invalid_messages;
                    validation_handler = m_validation_handler;

                    if (m_options.drop_invalid_entries) {
                        for (const auto& finding : validation.findings) {
                            if (finding.entry.empty()) {
                                // An undeclared module or set invalidates the whole envelope.
                                m_statistics.entries_dropped += envelope.values.size();
                                envelope.values.clear();
                                break;
                            }
                            if (envelope.values.erase(finding.entry) != 0) {
                                ++m_statistics.entries_dropped;
                            }
                        }
                        if (envelope.values.empty()) {
                            ++m_statistics.messages_dropped;
                            dispatchable = false;
                        }
                    }
                }
            }
        }
    }

    if (parse_error_handler) {
        parse_error_handler(topic, result);
    }
    if (validation_handler) {
        validation_handler(envelope, validation);
    }
    if (dispatchable) {
        dispatch(envelope);
    }
}

void ConsumerCore::dispatch(const Envelope& envelope) {
    std::vector<Callback> matching;
    {
        const std::lock_guard<std::mutex> lock{m_registry->mutex};
        for (const auto& [id, entry] : m_registry->entries) {
            if (entry.filter.matches(envelope)) {
                matching.push_back(entry.callback);
            }
        }
    }

    {
        const std::lock_guard<std::mutex> lock{m_mutex};
        if (matching.empty()) {
            ++m_statistics.unmatched_messages;
        } else {
            m_statistics.dispatches += matching.size();
        }
    }

    // Callbacks run without any lock held, so a subscriber may unsubscribe or subscribe from within
    // its own callback.
    for (const auto& callback : matching) {
        callback(envelope);
    }
}

} // namespace Everest::telemetry
