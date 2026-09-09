// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <utils/telemetry/types.hpp>

/// \file
/// \brief Transport-agnostic core of the telemetry consumer.
///
/// The core owns everything a consumer does with the value flow - subscriptions, filter matching,
/// catalog validation, dispatch and statistics - and knows nothing about MQTT. A transport binding
/// registers a single wildcard subscription (Everest::telemetry::TOPIC_WILDCARD) and hands every
/// payload to handle_message(). Unit tests drive the same entry point directly, which is why they
/// need neither a broker nor threads. This mirrors the split the framework already uses for the
/// configuration service (config_service_core).
namespace Everest::telemetry {

class ConsumerCore {
public:
    using Callback = std::function<void(const Envelope&)>;
    using SubscriptionId = std::uint64_t;
    using ParseErrorHandler = std::function<void(std::string_view topic, const ParseResult&)>;
    using ValidationHandler = std::function<void(const Envelope&, const ValidationResult&)>;

    struct Options {
        /// \brief Check every value against the declaration catalog before dispatch.
        bool validate_against_catalog{false};
        /// \brief Remove values with findings instead of dispatching them. Requires validation.
        bool drop_invalid_entries{false};
    };

    struct Statistics {
        std::uint64_t messages_received{0};   ///< payloads handed to the core
        std::uint64_t parse_errors{0};        ///< payloads that never became an Envelope
        std::uint64_t invalid_messages{0};    ///< envelopes with at least one validation finding
        std::uint64_t entries_dropped{0};     ///< values removed by drop_invalid_entries
        std::uint64_t messages_dropped{0};    ///< envelopes not dispatched because nothing was left
        std::uint64_t unmatched_messages{0};  ///< envelopes no subscription filter matched
        std::uint64_t dispatches{0};          ///< callback invocations
    };

    /// \brief Owns one subscription. Delivery stops when the handle is destroyed or reset.
    ///
    /// Move-only, and safe to outlive its core: the registry is held weakly.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription();

        /// \brief Ends the subscription now. Idempotent.
        void reset();

        /// \returns true while this handle refers to a live subscription
        bool active() const;

        SubscriptionId id() const {
            return m_id;
        }

    private:
        friend class ConsumerCore;
        struct Registry;
        Subscription(std::weak_ptr<Registry> registry, SubscriptionId id);

        std::weak_ptr<Registry> m_registry;
        SubscriptionId m_id{0};
    };

    ConsumerCore();
    explicit ConsumerCore(Options options);
    ~ConsumerCore();
    ConsumerCore(const ConsumerCore&) = delete;
    ConsumerCore& operator=(const ConsumerCore&) = delete;

    /// \brief Replaces the declaration catalog, normally fetched once at startup before any producer
    /// has published. Enumeration is what lets a consumer know the variable universe up front.
    void set_definitions(TelemetryDefinitions definitions);
    const TelemetryDefinitions& definitions() const;

    /// \brief Subscribes to the value flow, filtered in process.
    ///
    /// Filters never become broker subscriptions: one wildcard subscription feeds every subscriber.
    Subscription subscribe(Filter filter, Callback callback);

    /// \returns the number of live subscriptions
    std::size_t subscription_count() const;

    /// \brief Feeds one payload received on \p topic into the core.
    ///
    /// A malformed payload is counted and reported to the parse error handler; it never throws and
    /// never reaches a subscriber, so one broken producer cannot take a consumer down.
    void handle_message(std::string_view topic, const nlohmann::json& payload);
    void handle_message(std::string_view topic, std::string_view payload);

    void set_parse_error_handler(ParseErrorHandler handler);
    void set_validation_handler(ValidationHandler handler);

    Statistics statistics() const;

    /// \returns the single topic a transport binding has to subscribe to
    static std::string_view wildcard_topic() {
        return TOPIC_WILDCARD;
    }

private:
    void dispatch(const Envelope& envelope);

    struct Entry {
        SubscriptionId id{0};
        Filter filter;
        Callback callback;
    };

    std::shared_ptr<Subscription::Registry> m_registry;
    mutable std::mutex m_mutex;
    Options m_options;
    TelemetryDefinitions m_definitions;
    Statistics m_statistics;
    ParseErrorHandler m_parse_error_handler;
    ValidationHandler m_validation_handler;
};

} // namespace Everest::telemetry
