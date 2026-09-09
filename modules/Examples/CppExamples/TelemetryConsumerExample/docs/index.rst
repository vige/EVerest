.. _everest_modules_handwritten_TelemetryConsumerExample:

*************************
TelemetryConsumerExample
*************************

Subscribes to the EVerest telemetry value flow with the consumer library and prints every
envelope it receives to stdout. It declares no interfaces, so it can be added to any config.

Filter the flow with ``filter_module_id`` and ``filter_set``; an empty string means no filter.
