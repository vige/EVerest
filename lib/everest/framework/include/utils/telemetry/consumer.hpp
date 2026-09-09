// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <utils/mqtt_abstraction.hpp>
#include <utils/telemetry/types.hpp>

/// \file
/// \brief Consumer side of the EVerest telemetry feature.
///
/// A Filter is a broker subscription: its unset members become single-level wildcards, the broker
/// delivers only the topics that match, and nothing is filtered a second time in process. A consumer
/// that wants the fast livedata of every producer subscribes to one topic; a consumer that wants one
/// producer's diagnostics subscribes to another. Traffic a consumer did not ask for never reaches it.
namespace Everest::telemetry {

class Consumer {
public:
    using Callback = std::function<void(const Envelope&)>;

    /// \brief Owns one broker subscription. Delivery stops when the handle is destroyed or reset.
    ///
    /// Move-only, and must not outlive the Consumer it came from.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription();

        /// \brief Unsubscribes now. Idempotent.
        void reset();

        /// \returns true while this handle holds a subscription
        bool active() const {
            return m_token != nullptr;
        }

        /// \returns the topic this subscription is registered on
        const std::string& topic() const {
            return m_topic;
        }

    private:
        friend class Consumer;
        Subscription(MQTTAbstraction& mqtt, std::string topic, Token token);

        MQTTAbstraction* m_mqtt{nullptr};
        std::string m_topic;
        Token m_token;
    };

    explicit Consumer(MQTTAbstraction& mqtt);

    /// \brief Subscribes to the value flow the \p filter describes and calls \p callback for every
    /// envelope that arrives on it.
    ///
    /// A payload that cannot be read is ignored: the producer declares its telemetry in its manifest
    /// and the framework checks what it publishes, so a consumer does not validate values again.
    Subscription subscribe(const Filter& filter, Callback callback);

    /// \brief Stores the declaration catalog, normally fetched once at startup before any producer
    /// has published, so types and units are available without waiting for a sample.
    void set_definitions(TelemetryDefinitions definitions);
    const TelemetryDefinitions& definitions() const;

private:
    MQTTAbstraction& m_mqtt;
    TelemetryDefinitions m_definitions;
};

} // namespace Everest::telemetry
