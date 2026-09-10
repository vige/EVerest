#!/usr/bin/env bash
# A telemetry producer with no dependencies: one hand-assembled OTLP/HTTP protobuf request.
#
# The Python example beside this one is what a real driver looks like. This is the other end of the
# scale, and it is worth having: it shows that nothing on the receiving side needs an SDK, a
# protobuf runtime or a code generator. A shell script that can run curl can put a value on a CSMS.
#
#   ./telemetry-producer-example.sh              posts once
#   watch -n2 ./telemetry-producer-example.sh    keeps the value fresh
#
# The payload is one ExportMetricsServiceRequest:
#   resource   service.name = "my-service"
#   scope      "manual"
#   metric     system.memory.usage, unit By
#   gauge      one data point, as_int = 67, attribute state = "used"
#
# config/telemetry-mappings.yaml maps it onto Controller/MemoryUsed.
#
# It is base64 here rather than \x escapes on the curl command line because the message contains
# NUL bytes: bash truncates a $'...' string at the first one, and the receiver then -- correctly --
# answers 400 for a body that stops in the middle of a field.
set -euo pipefail

ENDPOINT="${OTEL_EXPORTER_OTLP_ENDPOINT:-http://127.0.0.1:4318}/v1/metrics"

base64 -d <<< 'Cm4KHgocCgxzZXJ2aWNlLm5hbWUSDAoKbXktc2VydmljZRJMCggKBm1hbnVhbBJAChNzeXN0ZW0ubWVtb3J5LnVzYWdlGgJCeSolCiMZAAAqNv6clxcxQwAAAAAAAAA6DwoFc3RhdGUSBgoEdXNlZA==' | curl -sS -X POST "$ENDPOINT"   -H 'Content-Type: application/x-protobuf'   --data-binary @-   -w '
HTTP %{http_code}
'
