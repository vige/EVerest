.. _everest_modules_handwritten_TelemetryProducerExample:

*************************
TelemetryProducerExample
*************************

A stub telemetry publisher, for developing and demonstrating the consumer side while the
producer-side framework helper does not exist yet.

It provides two implementations of the ``telemetry`` interface, ``livedata`` (capped at 4 Hz) and
``diagnostics`` (capped at 0.2 Hz), and drives them from a simulator thread instead of hardware.

``set_publisher.hpp`` implements what the framework will do later:

* an interest table keyed by subscriber, and publishing only the union over all of them,
* a last-value cache with exact-equality de-duplication, so an unchanged value is not re-published,
* a producer-side rate cap from ``max_publish_rate_hz``,
* a snapshot of the requested entries whenever a subscriber declares interest.

Nothing is published while no subscriber is interested, and the simulator skips sampling entirely in
that case.
