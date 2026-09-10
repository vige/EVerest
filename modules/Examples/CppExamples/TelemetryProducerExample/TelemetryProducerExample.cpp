// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "TelemetryProducerExample.hpp"

#include <chrono>
#include <cmath>

#include "telemetry.hpp"

namespace module {

void TelemetryProducerExample::init() {
    // Nothing to initialise. The OpenTelemetry client is generated from the manifest and installed
    // by the module loader before this runs.
    invoke_init(*p_main);
}

void TelemetryProducerExample::ready() {
    invoke_ready(*p_main);
    running = true;
    simulator = std::thread([this] { this->simulate(); });
}

void TelemetryProducerExample::simulate() {
    const auto interval = std::chrono::milliseconds(config.publish_interval_ms);
    unsigned tick = 0;

    while (running) {
        std::this_thread::sleep_for(interval);
        ++tick;

        // This is the whole producer side. Where these go, how often they leave the station and
        // under whose identity are the SDK's business, decided by the OTEL_* environment
        // variables; a driver states what it measured and stops there.
        const double phase = static_cast<double>(tick) / 10.0;
        telemetry().powermeter_temperature.record(40.0 + std::sin(phase), {{"evse", 1}});
        telemetry().powermeter_frequency.record(50.0 + 0.02 * std::cos(phase), {{"evse", 1}});
        telemetry().powermeter_current.record(std::round(1000.0 * std::abs(std::sin(phase / 3.0))) / 10.0,
                                              {{"evse", 1}});

        // A second EVSE on the same metric: one data point per EVSE, told apart by an attribute,
        // which is what an OCPP mapping file selects on.
        telemetry().powermeter_temperature.record(30.0 + std::sin(phase / 1.5), {{"evse", 2}});

        if (not config.live_only) {
            telemetry().powermeter_uptime.add(config.publish_interval_ms / 1000);
        }
    }
}

void TelemetryProducerExample::shutdown() {
    running = false;
    if (simulator.joinable()) {
        simulator.join();
    }
}

} // namespace module
