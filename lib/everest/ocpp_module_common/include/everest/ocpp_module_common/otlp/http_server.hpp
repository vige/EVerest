// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <everest/ocpp_module_common/otlp/metrics_decode.hpp>

/// \file
/// \brief The OTLP/HTTP receiving half: one route, one method, one content type.
///
/// An OpenTelemetry exporter configured with `OTEL_EXPORTER_OTLP_ENDPOINT` posts a serialised
/// ExportMetricsServiceRequest to `/v1/metrics`. That is the entire protocol surface this serves.
/// There is no read side: OTLP cannot be queried, so nothing here answers a GET.
namespace ocpp_module_common::otlp {

/// \brief What the server does with a request body it could not use.
enum class Rejection : std::uint8_t {
    NotFound,        ///< a path other than /v1/metrics
    MethodNotAllowed,///< anything but POST
    UnsupportedMedia,///< not application/x-protobuf, or an encoding we cannot undo
    TooLarge,        ///< a body over the configured limit
    Malformed        ///< a body that is not a well formed ExportMetricsServiceRequest
};

/// \brief An HTTP server that accepts OTLP metric exports.
///
/// Runs its own service thread. The handler is called on that thread, once per accepted request,
/// and must not block: while it runs, nothing else is being received. Writing a decoded batch into
/// a map under a mutex is the intended shape.
class HttpServer {
public:
    /// \param bind_address the interface to listen on. Loopback unless somebody has decided
    ///        otherwise: any process that can reach this can set a value a mapping turns into a
    ///        CSMS-visible variable
    /// \param port the TCP port
    /// \param max_body_bytes the largest request accepted; a collector batching many producers
    ///        sends more than a single driver does
    /// \param on_export called for each accepted export, on the service thread
    HttpServer(std::string bind_address, int port, std::size_t max_body_bytes,
               std::function<void(const Export&)> on_export);

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    /// \brief Stops the service thread and closes the listener.
    ~HttpServer();

    /// \brief Binds the port and starts serving.
    /// \returns false when the port could not be bound, which is not fatal to the module: the
    ///          station runs without telemetry rather than not at all
    bool start();

    /// \brief Stops serving. Safe to call more than once, and called by the destructor.
    void stop();

    bool running() const;

    /// \returns how many exports have been accepted since start
    std::uint64_t accepted() const;

    /// \returns how many requests have been turned away, for whatever reason
    std::uint64_t rejected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// \returns the reason as the text that goes in a log line
const char* to_string(Rejection rejection);

} // namespace ocpp_module_common::otlp
