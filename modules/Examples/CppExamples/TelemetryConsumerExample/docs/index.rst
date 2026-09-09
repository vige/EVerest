.. _everest_modules_handwritten_TelemetryConsumerExample:

*************************
TelemetryConsumerExample
*************************

A debugging telemetry sink. It logs the declaration of every telemetry set wired to it and every
update it is interested in, and it declares no interfaces of its own, so it can be added to any
config.

Telemetry travels as an ordinary interface variable, so the sets this module may consume are wired
to it in the ``connections`` block of ``config.yaml``, under the ``telemetry`` requirement. Which of
them it actually asks for is decided at runtime by the filter:

* ``filter_module_id``, ``filter_module_type``, ``filter_set`` — an empty string matches everything.
* ``filter_entries`` — comma separated entry names; an empty string means every entry the set
  declares.
* ``print_definitions`` — log what each set declared, with types and units.

The module subscribes in ``init`` and calls ``set_interest`` in ``ready``. That order matters: the
snapshot a publisher sends when interest changes is a normal update, so it is lost if no handler is
registered when it arrives.
