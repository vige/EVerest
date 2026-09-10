#!/usr/bin/env python3
"""A telemetry producer, as small as one can be: an OpenTelemetry SDK and a loop.

This is the whole producer side of the design. It is not an EVerest module, has no manifest, is
not wired to anything in config.yaml, and knows nothing about OCPP -- it exports metrics to an
OTLP endpoint, and the OCPP module turns the ones its mapping file names into device model
variables. A driver written in C++ does exactly this with the same protocol.

    uv venv --python 3.12 .venv
    uv pip install --python .venv/bin/python opentelemetry-sdk opentelemetry-exporter-otlp-proto-http
    OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318 .venv/bin/python telemetry-producer-example.py

Every setting is OpenTelemetry's own, so nothing here is EVerest-specific:

    OTEL_EXPORTER_OTLP_ENDPOINT   where to push, default http://127.0.0.1:4318
    OTEL_METRIC_EXPORT_INTERVAL   milliseconds between exports, default 2000 here
    OTEL_SERVICE_NAME             who is reporting, default powermeter_1

The receiver reads protobuf and does not inflate a compressed body, so leave
OTEL_EXPORTER_OTLP_COMPRESSION unset.
"""
import math
import os
import random
import signal
import time

from opentelemetry.exporter.otlp.proto.http.metric_exporter import OTLPMetricExporter
from opentelemetry.metrics import CallbackOptions, Observation, get_meter_provider, set_meter_provider
from opentelemetry.sdk.metrics import MeterProvider
from opentelemetry.sdk.metrics.export import PeriodicExportingMetricReader
from opentelemetry.sdk.resources import Resource

ENDPOINT = os.environ.get('OTEL_EXPORTER_OTLP_ENDPOINT', 'http://127.0.0.1:4318')
INTERVAL_MS = int(os.environ.get('OTEL_METRIC_EXPORT_INTERVAL', '2000'))
SERVICE = os.environ.get('OTEL_SERVICE_NAME', 'powermeter_1')

started = time.time()
running = True


def wave(period_s, low, high):
    """A value that moves, so a Delta monitor has something to fire on."""
    phase = (time.time() - started) / period_s * 2 * math.pi
    middle = (low + high) / 2
    return middle + (high - low) / 2 * math.sin(phase)


def temperature(options: CallbackOptions):
    # one observation per EVSE: the same metric, told apart by an attribute, which is how OTLP
    # expects a per-instance measurement to be shaped
    yield Observation(round(wave(60, 38.0, 43.0), 4), {'evse': 1})
    yield Observation(round(wave(90, 30.0, 35.0), 4), {'evse': 2})


def current(options: CallbackOptions):
    yield Observation(round(wave(20, 0.0, 32.0), 3), {'evse': 1})


def frequency(options: CallbackOptions):
    yield Observation(round(50.0 + random.uniform(-0.05, 0.05), 4), {'evse': 1})


def cpu_utilisation(options: CallbackOptions):
    # the point of the design in one callback: nothing in EVerest measures this, and it still
    # reaches the CSMS, because the mapping file names it
    try:
        load1, _, _ = os.getloadavg()
        yield Observation(round(min(load1 / (os.cpu_count() or 1) * 100.0, 100.0), 2))
    except OSError:
        yield Observation(0.0)


def memory_used(options: CallbackOptions):
    try:
        with open('/proc/meminfo', encoding='utf-8') as meminfo:
            values = {}
            for line in meminfo:
                key, _, rest = line.partition(':')
                values[key] = int(rest.strip().split()[0]) * 1024
        used = values['MemTotal'] - values.get('MemAvailable', values.get('MemFree', 0))
    except (OSError, KeyError, ValueError):
        used = 0
    yield Observation(used, {'state': 'used'})


def main():
    reader = PeriodicExportingMetricReader(
        OTLPMetricExporter(endpoint=f'{ENDPOINT.rstrip("/")}/v1/metrics'),
        export_interval_millis=INTERVAL_MS)
    set_meter_provider(MeterProvider(
        resource=Resource.create({'service.name': SERVICE}),
        metric_readers=[reader]))

    meter = get_meter_provider().get_meter('kempower.telemetry.example', '1.0.0')
    # observable instruments: the callback runs only when the reader collects, so a value nobody
    # exports is a value never measured
    meter.create_observable_gauge('powermeter.temperature', callbacks=[temperature],
                                  unit='Cel', description='Board temperature')
    meter.create_observable_gauge('powermeter.current', callbacks=[current],
                                  unit='A', description='Output current')
    meter.create_observable_gauge('powermeter.frequency', callbacks=[frequency],
                                  unit='Hz', description='Grid frequency')
    meter.create_observable_gauge('system.cpu.utilization', callbacks=[cpu_utilisation],
                                  unit='1', description='CPU load, as a percentage')
    meter.create_observable_gauge('system.memory.usage', callbacks=[memory_used],
                                  unit='By', description='Memory in use')

    print(f'exporting to {ENDPOINT}/v1/metrics every {INTERVAL_MS} ms as {SERVICE}; ctrl-c to stop',
          flush=True)

    def stop(*_):
        global running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    while running:
        time.sleep(0.2)

    reader.shutdown()


if __name__ == '__main__':
    main()
