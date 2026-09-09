// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "TelemetryProducerExample.hpp"

#include <chrono>
#include <cmath>

namespace module {

void TelemetryProducerExample::init() {
    invoke_init(*p_livedata);
    invoke_init(*p_diagnostics);
}

void TelemetryProducerExample::ready() {
    invoke_ready(*p_livedata);
    invoke_ready(*p_diagnostics);

    running = true;
    simulator = std::thread([this] { this->simulate(); });
}

void TelemetryProducerExample::simulate() {
    const auto interval = std::chrono::milliseconds(config.publish_interval_ms);
    unsigned tick = 0;

    while (running) {
        std::this_thread::sleep_for(interval);
        ++tick;

        if (livedata_publisher != nullptr and not livedata_publisher->wanted().empty()) {
            const double phase = static_cast<double>(tick) / 10.0;
            livedata_publisher->offer({
                {"temperature_C", 40.0 + std::sin(phase)},
                {"frequency_Hz", 50.0 + 0.02 * std::cos(phase)},
                {"current_A", std::round(100.0 * std::abs(std::sin(phase / 3.0))) / 10.0},
                {"fw_state", tick % 20 == 0 ? "Idle" : "Measuring"},
            });
        }

        // Diagnostics change slowly on purpose: de-duplication means most of these ticks publish
        // nothing at all, which is what the consumer side has to cope with.
        if (not config.live_only and diagnostics_publisher != nullptr and
            not diagnostics_publisher->wanted().empty()) {
            diagnostics_publisher->offer({
                {"uptime_s", static_cast<std::int64_t>(tick * config.publish_interval_ms / 1000)},
                {"error_count", 0},
                {"serial", "STUB-0001"},
            });
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
