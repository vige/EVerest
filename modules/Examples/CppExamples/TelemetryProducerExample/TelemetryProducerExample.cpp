// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "TelemetryProducerExample.hpp"

#include <chrono>
#include <cmath>

namespace module {

void TelemetryProducerExample::init() {
    // Nothing to initialise: both implementations are generated from the manifest declaration and
    // answer get_definition and set_interest on their own.
}

void TelemetryProducerExample::ready() {
    running = true;
    simulator = std::thread([this] { this->simulate(); });
}

void TelemetryProducerExample::simulate() {
    const auto interval = std::chrono::milliseconds(config.publish_interval_ms);
    unsigned tick = 0;

    while (running) {
        std::this_thread::sleep_for(interval);
        ++tick;

        // Nothing is published while nobody is interested, so nothing needs sampling either. This
        // is the one place a driver has to care about interest, and only to save its own work.
        if (p_livedata->any_interest()) {
            const double phase = static_cast<double>(tick) / 10.0;
            livedata::Sample sample;
            sample.temperature_C = 40.0 + std::sin(phase);
            sample.frequency_Hz = 50.0 + 0.02 * std::cos(phase);
            sample.current_A = std::round(100.0 * std::abs(std::sin(phase / 3.0))) / 10.0;
            sample.fw_state = tick % 20 == 0 ? livedata::FwState::Idle : livedata::FwState::Measuring;
            p_livedata->publish(sample);
        }

        // Diagnostics change slowly on purpose: de-duplication means most of these ticks publish
        // nothing at all, which is what the consumer side has to cope with.
        if (not config.live_only and p_diagnostics->any_interest()) {
            diagnostics::Sample sample;
            sample.uptime_s = static_cast<int>(tick * config.publish_interval_ms / 1000);
            sample.error_count = 0;
            sample.serial = "STUB-0001";
            p_diagnostics->publish(sample);
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
