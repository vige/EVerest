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

        // publish() is a no-op while nobody is interested, so there is no guard here. A driver
        // whose sampling costs a bus transaction can ask any_interest() first and skip that work;
        // reading a sine wave is not worth the branch.
        const double phase = static_cast<double>(tick) / 10.0;
        livedata::Sample live;
        live.temperature_C = 40.0 + std::sin(phase);
        live.frequency_Hz = 50.0 + 0.02 * std::cos(phase);
        live.current_A = std::round(100.0 * std::abs(std::sin(phase / 3.0))) / 10.0;
        live.fw_state = tick % 20 == 0 ? livedata::FwState::Idle : livedata::FwState::Measuring;
        p_livedata->publish(live);

        // Diagnostics change slowly on purpose: de-duplication means most of these ticks publish
        // nothing at all, which is what the consumer side has to cope with.
        if (not config.live_only) {
            diagnostics::Sample slow;
            slow.uptime_s = static_cast<int>(tick * config.publish_interval_ms / 1000);
            slow.error_count = 0;
            slow.serial = "STUB-0001";
            p_diagnostics->publish(slow);
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
