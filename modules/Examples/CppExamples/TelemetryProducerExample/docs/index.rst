.. _everest_modules_handwritten_TelemetryProducerExample:

*************************
TelemetryProducerExample
*************************

A telemetry publisher whose measurements are declared in the top-level ``telemetry`` block of its
manifest. It samples a simulator instead of hardware, so it stands in for a driver without needing
one.

The OpenTelemetry client is generated from that declaration and installed by the module loader
before any implementation initialises. The module holds no exporter, configures no endpoint and
writes no export code. Where the measurements go, how often they leave the station and under whose
identity are decided by the ``OTEL_*`` environment variables the SDK reads.

What the module writes is the measurement:

.. code-block:: cpp

   telemetry().powermeter_temperature.record(40.0 + std::sin(phase), {{"evse", 1}});

One metric carries a data point per EVSE, told apart by an attribute, which is what an OCPP mapping
file selects on. Counters use ``add()`` instead of ``record()``.

Built without ``EVEREST_ENABLE_OTLP_TELEMETRY`` the same calls compile to no-ops, so the module's
own source never changes between the two builds.

Config:

* ``publish_interval_ms`` - how often the simulator samples. The export interval applies on top.
* ``live_only`` - sample the live electrical measurements only, leaving the slow ones alone.
