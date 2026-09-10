#!/usr/bin/env python3
"""Regenerate the OTLP fixtures beside this script, using a real OpenTelemetry SDK.

The decoder is hand written, so a test that builds its own protobuf would only prove the decoder
agrees with itself. These fixtures come from the same library a driver would use, which is what
makes the tests evidence rather than a tautology.

    uv venv --python 3.12 .venv
    uv pip install --python .venv/bin/python opentelemetry-sdk opentelemetry-exporter-otlp-proto-http
    .venv/bin/python generate.py
"""
import pathlib

from opentelemetry.exporter.otlp.proto.common._internal.metrics_encoder import encode_metrics
from opentelemetry.sdk.metrics.export import (AggregationTemporality, Gauge, Metric,
                                              MetricsData, NumberDataPoint, ResourceMetrics,
                                              ScopeMetrics, Sum)
from opentelemetry.sdk.resources import Resource
from opentelemetry.sdk.util.instrumentation import InstrumentationScope

HERE = pathlib.Path(__file__).parent
T0 = 1_757_500_000_000_000_000  # a fixed timestamp, so the fixtures are reproducible


def point(value, attributes=None, start=T0, time=T0):
    return NumberDataPoint(attributes=attributes or {}, start_time_unix_nano=start,
                           time_unix_nano=time, value=value)


def metric(name, description, unit, data):
    return Metric(name=name, description=description, unit=unit, data=data)


def write(name, metrics_data):
    body = encode_metrics(metrics_data).SerializeToString()
    (HERE / name).write_bytes(body)
    print(f'{name}  {len(body)} bytes')


def data(resource, metrics, scope_name='kempower.telemetry'):
    return MetricsData(resource_metrics=[ResourceMetrics(
        resource=resource,
        scope_metrics=[ScopeMetrics(
            scope=InstrumentationScope(scope_name, '1.0.0'),
            metrics=metrics,
            schema_url='')],
        schema_url='')])


resource = Resource.create({'service.name': 'powermeter_1', 'station.serial': 'KP-0001'})

# a double gauge with a resource attribute and a data point attribute
write('gauge_double.bin', data(resource, [
    metric('powermeter.temperature', 'Board temperature', 'Cel',
           Gauge(data_points=[point(41.5, {'evse': 1})])),
]))

# an integer gauge, an integer sum, and a metric with no unit
write('mixed.bin', data(resource, [
    metric('powermeter.temperature', 'Board temperature', 'Cel',
           Gauge(data_points=[point(41.5, {'evse': 1}), point(38.25, {'evse': 2})])),
    metric('system.uptime', 'Seconds since boot', 's',
           Gauge(data_points=[point(86400)])),
    metric('powermeter.energy_imported', 'Cumulative imported energy', 'W.h',
           Sum(data_points=[point(12345, {'evse': 1})],
               aggregation_temporality=AggregationTemporality.CUMULATIVE,
               is_monotonic=True)),
    metric('modem.rsrp', '', '',
           Gauge(data_points=[point(-97, {'imei': '350000000000001', 'roaming': False})])),
]))

# attribute value types: string, bool, int, double
write('attribute_types.bin', data(resource, [
    metric('probe.value', 'Every attribute value type', '1',
           Gauge(data_points=[point(1.0, {'text': 'measuring', 'flag': True,
                                          'count': 7, 'ratio': 0.25})])),
]))

# a data point attribute shadowing a resource attribute of the same name
write('shadowed_attribute.bin', data(
    Resource.create({'service.name': 'powermeter_1', 'evse': 0}),
    [metric('powermeter.current', 'Output current', 'A',
            Gauge(data_points=[point(5.9, {'evse': 1})]))]))

# an empty export: a producer with nothing to report
write('empty.bin', MetricsData(resource_metrics=[]))
