// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/ocpp_module_common/otlp/http_server.hpp>

#include <array>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include <libwebsockets.h>

#include <everest/logging.hpp>

namespace ocpp_module_common::otlp {

namespace {

constexpr auto OTLP_METRICS_PATH = "/v1/metrics";
constexpr auto CONTENT_TYPE_PROTOBUF = "application/x-protobuf";

/// libwebsockets asks for a header buffer up front; a content type nobody would send is longer
/// than this, and a truncated read is treated as a content type we do not accept.
constexpr std::size_t HEADER_BUFFER = 256;

/// Room for the response headers. The body is always empty, so this is all the space needed.
constexpr std::size_t RESPONSE_BUFFER = 512;

/// How long lws_service waits before returning, so a stop is noticed promptly without spinning.
constexpr int SERVICE_TIMEOUT_MS = 50;

/// \brief What is known about one request while its body is still arriving.
struct Pending {
    std::string body;
    bool refuse{false};        ///< headers already disqualified it; drain the body and answer
    Rejection reason{Rejection::Malformed};
};

} // namespace

const char* to_string(Rejection rejection) {
    switch (rejection) {
    case Rejection::NotFound:
        return "unknown path";
    case Rejection::MethodNotAllowed:
        return "method not allowed";
    case Rejection::UnsupportedMedia:
        return "unsupported content type or encoding";
    case Rejection::TooLarge:
        return "body too large";
    case Rejection::Malformed:
        return "malformed OTLP request";
    }
    return "unknown";
}

struct HttpServer::Impl {
    Impl(std::string bind_address, int port, std::size_t max_body_bytes,
         std::function<void(const Export&)> on_export) :
        address(std::move(bind_address)),
        port(port),
        max_body_bytes(max_body_bytes),
        on_export(std::move(on_export)) {
    }

    std::string address;
    int port;
    std::size_t max_body_bytes;
    std::function<void(const Export&)> on_export;

    lws_context* context{nullptr};
    std::thread service_thread;
    std::atomic<bool> serving{false};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};

    /// Bodies in flight, keyed by connection. Only ever touched from the service thread, which is
    /// why there is no lock: lws runs every callback for every connection on that one thread.
    std::unordered_map<lws*, Pending> pending;

    /// \brief Answers with a status and no body, and ends the transaction.
    /// \returns the value the lws callback should return
    static int respond(lws* wsi, unsigned int status) {
        std::array<unsigned char, LWS_PRE + RESPONSE_BUFFER> buffer{};
        auto* start = buffer.data() + LWS_PRE;
        auto* p = start;
        auto* end = buffer.data() + buffer.size() - 1;

        if (lws_add_http_header_status(wsi, status, &p, end) != 0) {
            return 1;
        }
        // a success is an empty ExportMetricsServiceResponse, which serialises to zero bytes, so
        // the content type is honest even though nothing follows it
        if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                         reinterpret_cast<const unsigned char*>(CONTENT_TYPE_PROTOBUF),
                                         static_cast<int>(std::strlen(CONTENT_TYPE_PROTOBUF)), &p, end) != 0) {
            return 1;
        }
        if (lws_add_http_header_content_length(wsi, 0, &p, end) != 0) {
            return 1;
        }
        if (lws_finalize_write_http_header(wsi, start, &p, end) != 0) {
            return 1;
        }
        return lws_http_transaction_completed(wsi) ? -1 : 0;
    }

    static unsigned int status_of(Rejection rejection) {
        switch (rejection) {
        case Rejection::NotFound:
            return HTTP_STATUS_NOT_FOUND;
        case Rejection::MethodNotAllowed:
            return HTTP_STATUS_METHOD_NOT_ALLOWED;
        case Rejection::UnsupportedMedia:
            return HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE;
        case Rejection::TooLarge:
            return HTTP_STATUS_REQ_ENTITY_TOO_LARGE;
        case Rejection::Malformed:
            return HTTP_STATUS_BAD_REQUEST;
        }
        return HTTP_STATUS_BAD_REQUEST;
    }

    int refuse(lws* wsi, Rejection reason) {
        this->rejected += 1;
        EVLOG_debug << "otlp: refused a request: " << to_string(reason);
        return respond(wsi, status_of(reason));
    }

    /// \brief Decides from the headers whether this request can be accepted at all.
    std::optional<Rejection> check_headers(lws* wsi, const char* uri) {
        if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) == 0) {
            return Rejection::MethodNotAllowed;
        }
        if (uri == nullptr or std::strcmp(uri, OTLP_METRICS_PATH) != 0) {
            return Rejection::NotFound;
        }

        std::array<char, HEADER_BUFFER> header{};
        const auto type_length =
            lws_hdr_copy(wsi, header.data(), static_cast<int>(header.size()), WSI_TOKEN_HTTP_CONTENT_TYPE);
        if (type_length <= 0 or std::strncmp(header.data(), CONTENT_TYPE_PROTOBUF,
                                             std::strlen(CONTENT_TYPE_PROTOBUF)) != 0) {
            // JSON is a valid OTLP encoding this receiver does not read; saying so with a 415 is
            // more use to whoever configured the exporter than a decode failure would be
            return Rejection::UnsupportedMedia;
        }

        // a compressed body would have to be inflated before it could be decoded, and nothing here
        // links zlib; an exporter has to be configured without compression
        header.fill('\0');
        if (lws_hdr_copy(wsi, header.data(), static_cast<int>(header.size()),
                         WSI_TOKEN_HTTP_CONTENT_ENCODING) > 0) {
            const std::string encoding(header.data());
            if (not encoding.empty() and encoding != "identity") {
                return Rejection::UnsupportedMedia;
            }
        }
        return std::nullopt;
    }

    int on_http(lws* wsi, const char* uri) {
        Pending state;
        if (const auto reason = check_headers(wsi, uri)) {
            // the body still has to be read off the socket before the transaction can end, so the
            // verdict is remembered and applied at completion
            state.refuse = true;
            state.reason = *reason;
        }
        this->pending[wsi] = std::move(state);
        return 0;
    }

    int on_body(lws* wsi, const void* in, std::size_t len) {
        const auto found = this->pending.find(wsi);
        if (found == this->pending.end()) {
            return 0;
        }
        auto& state = found->second;
        if (state.refuse) {
            return 0; // draining
        }
        if (state.body.size() + len > this->max_body_bytes) {
            state.refuse = true;
            state.reason = Rejection::TooLarge;
            state.body.clear();
            state.body.shrink_to_fit();
            return 0;
        }
        state.body.append(static_cast<const char*>(in), len);
        return 0;
    }

    int on_body_complete(lws* wsi) {
        const auto found = this->pending.find(wsi);
        if (found == this->pending.end()) {
            return refuse(wsi, Rejection::Malformed);
        }
        Pending state = std::move(found->second);
        this->pending.erase(found);

        if (state.refuse) {
            return refuse(wsi, state.reason);
        }

        const auto decoded = decode_export_metrics_request(state.body);
        if (not decoded.has_value()) {
            return refuse(wsi, Rejection::Malformed);
        }

        this->accepted += 1;
        if (this->on_export) {
            this->on_export(*decoded);
        }
        return respond(wsi, HTTP_STATUS_OK);
    }

    void on_closed(lws* wsi) {
        this->pending.erase(wsi);
    }

    static int callback(lws* wsi, lws_callback_reasons reason, void* /*user*/, void* in, std::size_t len) {
        const auto* protocol = lws_get_protocol(wsi);
        auto* self = (protocol != nullptr) ? static_cast<Impl*>(protocol->user) : nullptr;
        if (self == nullptr) {
            return 0;
        }

        // an if chain rather than a switch: this library is built with -Werror=switch-enum, and
        // lws_callback_reasons has some two hundred enumerators, all but four of them irrelevant
        if (reason == LWS_CALLBACK_HTTP) {
            return self->on_http(wsi, static_cast<const char*>(in));
        }
        if (reason == LWS_CALLBACK_HTTP_BODY) {
            return self->on_body(wsi, in, len);
        }
        if (reason == LWS_CALLBACK_HTTP_BODY_COMPLETION) {
            return self->on_body_complete(wsi);
        }
        if (reason == LWS_CALLBACK_CLOSED_HTTP or reason == LWS_CALLBACK_HTTP_DROP_PROTOCOL) {
            self->on_closed(wsi);
        }
        return 0;
    }
};

HttpServer::HttpServer(std::string bind_address, int port, std::size_t max_body_bytes,
                       std::function<void(const Export&)> on_export) :
    m_impl(std::make_unique<Impl>(std::move(bind_address), port, max_body_bytes, std::move(on_export))) {
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (m_impl->serving) {
        return true;
    }

    // the protocol list is owned by the context for as long as it lives, so it cannot be a local
    static thread_local std::vector<lws_protocols> protocols;
    protocols.clear();
    lws_protocols otlp{};
    otlp.name = "otlp-http";
    otlp.callback = &Impl::callback;
    otlp.per_session_data_size = 0;
    otlp.rx_buffer_size = 0;
    otlp.user = m_impl.get();
    protocols.push_back(otlp);
    protocols.push_back(lws_protocols{});

    lws_context_creation_info info{};
    info.port = m_impl->port;
    info.iface = m_impl->address.empty() ? nullptr : m_impl->address.c_str();
    info.protocols = protocols.data();
    info.gid = static_cast<gid_t>(-1);
    info.uid = static_cast<uid_t>(-1);
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

    m_impl->context = lws_create_context(&info);
    if (m_impl->context == nullptr) {
        EVLOG_error << "otlp: could not listen on " << m_impl->address << ":" << m_impl->port
                    << "; telemetry will not be received";
        return false;
    }

    m_impl->serving = true;
    m_impl->service_thread = std::thread([this]() {
        while (m_impl->serving) {
            // a negative return means the context wants to go away, and continuing to service it
            // would spin on a dead context
            if (lws_service(m_impl->context, SERVICE_TIMEOUT_MS) < 0) {
                break;
            }
        }
    });

    EVLOG_info << "otlp: listening on " << m_impl->address << ":" << m_impl->port << OTLP_METRICS_PATH;
    return true;
}

void HttpServer::stop() {
    if (not m_impl->serving.exchange(false)) {
        // never started, or already stopped; the context still has to go if start() half succeeded
        if (m_impl->context != nullptr) {
            lws_context_destroy(m_impl->context);
            m_impl->context = nullptr;
        }
        return;
    }

    // wakes the service loop so the thread notices the flag rather than waiting out its timeout
    lws_cancel_service(m_impl->context);
    if (m_impl->service_thread.joinable()) {
        m_impl->service_thread.join();
    }
    lws_context_destroy(m_impl->context);
    m_impl->context = nullptr;
    EVLOG_info << "otlp: stopped after " << m_impl->accepted << " accepted and " << m_impl->rejected
               << " refused request(s)";
}

bool HttpServer::running() const {
    return m_impl->serving;
}

std::uint64_t HttpServer::accepted() const {
    return m_impl->accepted;
}

std::uint64_t HttpServer::rejected() const {
    return m_impl->rejected;
}

} // namespace ocpp_module_common::otlp
