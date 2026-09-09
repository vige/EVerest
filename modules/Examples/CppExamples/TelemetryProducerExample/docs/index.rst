.. _everest_modules_handwritten_TelemetryProducerExample:

*************************
TelemetryProducerExample
*************************

A telemetry publisher whose two sets, ``livedata`` and ``diagnostics``, are declared in its
manifest. It samples a simulator instead of hardware, so it stands in for a driver without needing
one.

There is no implementation directory. Both implementations are generated from the manifest
declaration, which means the module answers neither ``get_definition`` nor ``set_interest``, and
keeps no interest table, no last-value cache and no rate cap of its own. All of that lives in
``Everest::telemetry::SetPublisher``.

What the module writes is the sampling:

.. code-block:: cpp

   livedata::Sample sample;
   sample.temperature_C = 40.0 + std::sin(phase);
   sample.fw_state = livedata::FwState::Measuring;
   p_livedata->publish(sample);

``Sample``, the ``FwState`` enum and the per-entry ``publish_<entry>()`` overloads all come from the
``telemetry`` block in ``manifest.yaml``.

The one thing a driver still sees of the protocol is ``p_livedata->any_interest()``, and only so it
can skip sampling its own hardware while nobody is listening.

Config:

* ``publish_interval_ms`` - how often the simulator samples. Each set's ``max_publish_rate_hz``
  applies on top.
* ``live_only`` - sample the livedata set only, leaving diagnostics silent.
