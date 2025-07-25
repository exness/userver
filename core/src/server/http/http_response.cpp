#include <userver/server/http/http_response.hpp>

#include <array>

#include <cctz/time_zone.h>
#include <fmt/compile.h>

#include <userver/engine/deadline.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/hostinfo/blocking/get_hostname.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/tracing/set_throttle_reason.hpp>
#include <userver/tracing/span.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime/wall_coarse_clock.hpp>
#include <userver/utils/overloaded.hpp>
#include <userver/utils/small_string.hpp>

#include <server/http/http_cached_date.hpp>

#include <userver/server/http/http_request.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kKeyValueHeaderSeparator = ": ";

constexpr std::string_view kClose = "close";
constexpr std::string_view kKeepAlive = "keep-alive";

const std::string kHostname = hostinfo::blocking::GetRealHostName();

void CheckHeaderName(std::string_view name) {
    static constexpr auto init = []() {
        std::array<uint8_t, 256> res{};  // zero initialize
        for (int i = 0; i < 32; i++) res[i] = 1;
        for (int i = 127; i < 256; i++) res[i] = 1;
        for (const unsigned char c : "()<>@,;:\\\"/[]?={} \t") res[c] = 1;
        return res;
    };
    static constexpr auto bad_chars = init();

    for (const char c : name) {
        auto code = static_cast<uint8_t>(c);
        if (bad_chars[code]) {
            throw std::runtime_error(
                std::string("invalid character in header name: '") + c + "' (#" + std::to_string(code) + ")"
            );
        }
    }
}

void CheckHeaderValue(std::string_view value) {
    bool check_failed = false;

    // this gets autovectorized, and we optimize for happy path here
    for (const char c : value) {
        auto code = static_cast<uint8_t>(c);
        check_failed |= code < 32 || code == 127;
    }

    if (check_failed) {
        // in a presumably rare scenarios of the check failing we do a second loop
        for (const char c : value) {
            auto code = static_cast<uint8_t>(c);
            if (code < 32 || code == 127) {
                throw std::runtime_error(
                    std::string("invalid character in header value: '") + c + "' (#" + std::to_string(code) + ")"
                );
            }
        }
    }
}

bool IsBodyForbiddenForStatus(server::http::HttpStatus status) {
    return status == server::http::HttpStatus::kNoContent || status == server::http::HttpStatus::kNotModified ||
           (static_cast<int>(status) >= 100 && static_cast<int>(status) < 200);
}

void AppendToCharArray(char*& data, const std::string_view what) {
    std::memcpy(data, what.begin(), what.size());
    data += what.size();
}

const std::string kEmptyString{};

}  // namespace

namespace server::http {

namespace impl {

void OutputHeader(USERVER_NAMESPACE::http::headers::HeadersString& header, std::string_view key, std::string_view val) {
    const auto old_size = header.size();

    header.resize_and_overwrite(
        old_size + key.size() + kKeyValueHeaderSeparator.size() + val.size() + kCrlf.size(),
        [&](char* data, std::size_t size) {
            data += old_size;
            AppendToCharArray(data, key);
            AppendToCharArray(data, kKeyValueHeaderSeparator);
            AppendToCharArray(data, val);
            AppendToCharArray(data, kCrlf);
            return size;
        }
    );
}

}  // namespace impl

HttpResponse::HttpResponse(const HttpRequest& request, request::ResponseDataAccounter& data_accounter)
    : HttpResponse{request, data_accounter, std::chrono::steady_clock::now(), utils::StrCaseHash{}} {}

HttpResponse::HttpResponse(
    const HttpRequest& request,
    request::ResponseDataAccounter& data_accounter,
    std::chrono::steady_clock::time_point now,
    utils::StrCaseHash hasher
)
    : ResponseBase{data_accounter, now}, request_{request}, cookies_{0, hasher} {}

HttpResponse::~HttpResponse() = default;

void HttpResponse::SetSendFailed(std::chrono::steady_clock::time_point failure_time) {
    SetStatus(HttpStatus::kClientClosedRequest);
    request::ResponseBase::SetSendFailed(failure_time);
}

bool HttpResponse::SetHeader(std::string name, std::string value) {
    if (headers_end_.IsReady()) {
        // Attempt to set headers for Stream'ed response after it is already set
        return false;
    }

    CheckHeaderName(name);
    CheckHeaderValue(value);

    headers_.insert_or_assign(std::move(name), std::move(value));

    return true;
}

bool HttpResponse::SetHeader(std::string_view name, std::string value) {
    return SetHeader(std::string{name}, std::move(value));
}

bool HttpResponse::SetHeader(const USERVER_NAMESPACE::http::headers::PredefinedHeader& header, std::string value) {
    if (headers_end_.IsReady()) {
        // Attempt to set headers for Stream'ed response after it is already set
        return false;
    }

    CheckHeaderValue(value);

    headers_.insert_or_assign(header, std::move(value));

    return true;
}

void HttpResponse::SetContentType(const USERVER_NAMESPACE::http::ContentType& type) {
    SetHeader(USERVER_NAMESPACE::http::headers::kContentType, type.ToString());
}

void HttpResponse::SetContentEncoding(std::string encoding) {
    SetHeader(USERVER_NAMESPACE::http::headers::kContentEncoding, std::move(encoding));
}

bool HttpResponse::SetStatus(HttpStatus status) {
    if (headers_end_.IsReady()) {
        // Attempt to set headers for Stream'ed response after it is already set
        return false;
    }

    status_ = status;
    return true;
}

bool HttpResponse::ClearHeaders() {
    if (headers_end_.IsReady()) {
        // Attempt to set headers for Stream'ed response after it is already set
        return false;
    }

    headers_.clear();
    return true;
}

void HttpResponse::SetCookie(Cookie cookie) {
    CheckHeaderValue(cookie.Name());
    CheckHeaderValue(cookie.Value());
    UASSERT(!cookie.Name().empty());
    auto [it, ok] = cookies_.emplace(std::string_view{}, std::move(cookie));
    UASSERT(ok);

    auto node = cookies_.extract(it);
    UASSERT(node);
    node.key() = node.mapped().Name();
    cookies_.insert(std::move(node));
}

void HttpResponse::ClearCookies() { cookies_.clear(); }

HttpResponse::HeadersMapKeys HttpResponse::GetHeaderNames() const { return HttpResponse::HeadersMapKeys{headers_}; }

const std::string& HttpResponse::GetHeader(std::string_view header_name) const {
    auto it = headers_.find(header_name);
    if (it == headers_.end()) return kEmptyString;
    return it->second;
}

const std::string& HttpResponse::GetHeader(const USERVER_NAMESPACE::http::headers::PredefinedHeader& header_name
) const {
    auto it = headers_.find(header_name);
    if (it == headers_.end()) return kEmptyString;
    return it->second;
}

bool HttpResponse::HasHeader(std::string_view header_name) const {
    return headers_.find(header_name) != headers_.end();
}

bool HttpResponse::HasHeader(const USERVER_NAMESPACE::http::headers::PredefinedHeader& header_name) const {
    return headers_.find(header_name) != headers_.end();
}

HttpResponse::CookiesMapKeys HttpResponse::GetCookieNames() const { return HttpResponse::CookiesMapKeys{cookies_}; }

const Cookie& HttpResponse::GetCookie(std::string_view cookie_name) const { return cookies_.at(cookie_name.data()); }

void HttpResponse::SetHeadersEnd() { headers_end_.Send(); }

bool HttpResponse::WaitForHeadersEnd() { return headers_end_.WaitForEvent(); }

void HttpResponse::SendResponse(engine::io::RwBase& socket) {
    utils::SmallString<USERVER_NAMESPACE::http::headers::kTypicalHeadersSize> header;

    header.resize_and_overwrite(USERVER_NAMESPACE::http::headers::kTypicalHeadersSize, [&](char* data, std::size_t) {
        char* old_data_pointer = data;
        AppendToCharArray(data, "HTTP/");
        data = fmt::format_to(
            data, FMT_COMPILE("{}.{} {} "), request_.GetHttpMajor(), request_.GetHttpMinor(), static_cast<int>(status_)
        );
        AppendToCharArray(data, StatusCodeString(status_));
        AppendToCharArray(data, kCrlf);
        return data - old_data_pointer;
    });

    headers_.erase(USERVER_NAMESPACE::http::headers::kContentLength);
    const auto end = headers_.end();
    if (headers_.find(USERVER_NAMESPACE::http::headers::kDate) == end) {
        impl::OutputHeader(
            header,
            USERVER_NAMESPACE::http::headers::kDate,
            // impl::GetCachedDate() must not cross thread boundaries
            impl::GetCachedDate()
        );
    }
    if (headers_.find(USERVER_NAMESPACE::http::headers::kContentType) == end) {
        impl::OutputHeader(header, USERVER_NAMESPACE::http::headers::kContentType, kDefaultContentType);
    }
    headers_.OutputInHttpFormat(header);
    if (headers_.find(USERVER_NAMESPACE::http::headers::kConnection) == end) {
        impl::OutputHeader(
            header, USERVER_NAMESPACE::http::headers::kConnection, (request_.IsFinal() ? kClose : kKeepAlive)
        );
    }
    for (const auto& cookie : cookies_) {
        const std::size_t old_size = header.size();

        header.resize_and_overwrite(
            old_size + static_cast<std::string_view>(USERVER_NAMESPACE::http::headers::kSetCookie).size() +
                kKeyValueHeaderSeparator.size(),
            [&](char* data, std::size_t size) {
                data += old_size;
                AppendToCharArray(data, USERVER_NAMESPACE::http::headers::kSetCookie);
                AppendToCharArray(data, kKeyValueHeaderSeparator);
                return size;
            }
        );

        cookie.second.AppendToString(header);

        header.append(kCrlf);
    }

    std::size_t sent_bytes{};

    if (IsBodyStreamed() && GetData().empty()) {
        sent_bytes = SetBodyStreamed(socket, header);
    } else {
        // e.g. a CustomHandlerException
        sent_bytes = SetBodyNotStreamed(socket, header);
    }

    SetSent(sent_bytes, std::chrono::steady_clock::now());
}

std::size_t
HttpResponse::SetBodyNotStreamed(engine::io::RwBase& socket, USERVER_NAMESPACE::http::headers::HeadersString& header) {
    const bool is_body_forbidden = IsBodyForbiddenForStatus(status_);
    const bool is_head_request = request_.GetMethod() == HttpMethod::kHead;
    const auto& data = GetData();

    if (!is_body_forbidden) {
        impl::OutputHeader(
            header, USERVER_NAMESPACE::http::headers::kContentLength, fmt::format(FMT_COMPILE("{}"), data.size())
        );
    }
    header.append(kCrlf);

    if (is_body_forbidden && !data.empty()) {
        LOG_LIMITED_WARNING() << "Non-empty body provided for response with HTTP code " << static_cast<int>(status_)
                              << " which does not allow one, it will be dropped";
    }

    ssize_t sent_bytes = 0;
    if (!is_head_request && !is_body_forbidden) {
        sent_bytes = socket.WriteAll({{header.data(), header.size()}, {data.data(), data.size()}}, engine::Deadline{});
    } else {
        sent_bytes = socket.WriteAll(header.data(), header.size(), engine::Deadline{});
    }

    return sent_bytes;
}

std::size_t
HttpResponse::SetBodyStreamed(engine::io::RwBase& socket, USERVER_NAMESPACE::http::headers::HeadersString& header) {
    const bool is_body_forbidden = IsBodyForbiddenForStatus(status_);

    impl::OutputHeader(header, USERVER_NAMESPACE::http::headers::kTransferEncoding, "chunked");

    // headers end marker
    header.append(kCrlf);

    // send HTTP headers
    size_t sent_bytes = socket.WriteAll(header.data(), header.size(), {});
    header.clear();
    header.shrink_to_fit();  // free memory before time-consuming operation

    if (is_body_forbidden) {
        return sent_bytes;
    }

    // Transmit HTTP response body
    std::string body_part;
    // First chunk must be sent without kCrlf
    // because kCrlf was sent with headers
    bool first_chunk_processed = false;
    while (body_stream_->Pop(body_part)) {
        if (body_part.empty()) {
            LOG_DEBUG() << "Zero size body_part in http_response.cpp";
            continue;
        }

        auto size = first_chunk_processed ? fmt::format("\r\n{:x}\r\n", body_part.size())
                                          : fmt::format("{:x}\r\n", body_part.size());
        sent_bytes +=
            socket.WriteAll({{size.data(), size.size()}, {body_part.data(), body_part.size()}}, engine::Deadline{});

        first_chunk_processed = true;
    }

    const std::string_view terminating_chunk{first_chunk_processed ? "\r\n0\r\n\r\n" : "0\r\n\r\n"};
    sent_bytes += socket.WriteAll(terminating_chunk.data(), terminating_chunk.size(), {});

    // TODO: exceptions?
    body_stream_producer_.emplace<std::monostate>();
    body_stream_.reset();

    return sent_bytes;
}

void SetThrottleReason(http::HttpResponse& http_response, std::string log_reason, std::string http_header_reason) {
    http_response.SetHeader(USERVER_NAMESPACE::http::headers::kXYaTaxiRatelimitedBy, kHostname);
    http_response.SetHeader(USERVER_NAMESPACE::http::headers::kXYaTaxiRatelimitReason, std::move(http_header_reason));

    if (auto* span = tracing::Span::CurrentSpanUnchecked()) {
        tracing::SetThrottleReason(*span, std::move(log_reason));
    }
}

void HttpResponse::SetStreamBody() {
    UASSERT(body_stream_producer_.index() == 0);
    if (GetStreamId().has_value()) {
        body_stream_producer_.emplace<impl::Http2StreamEventProducer>(GetStreamProducer());
    } else {
        UASSERT(!body_stream_);
        const auto body_queue = Queue::Create();
        body_stream_.emplace(body_queue->GetConsumer());
        body_stream_producer_.emplace<Queue::Producer>(body_queue->GetProducer());
    }
    is_stream_body_ = true;
}

bool HttpResponse::IsBodyStreamed() const { return is_stream_body_; }

HttpResponse::Producer HttpResponse::GetBodyProducer() {
    Producer res{};
    std::visit(
        utils::Overloaded{
            [&res](Queue::Producer& p) mutable { res = std::move(p); },
            [&res](impl::Http2StreamEventProducer& p) mutable {
                res.emplace<impl::Http2StreamEventProducer>(std::move(p));
            },
            [](const std::monostate) mutable { UINVARIANT(false, "GetBodyProducer() is called twice"); },

        },
        body_stream_producer_
    );
    body_stream_producer_.emplace<std::monostate>();
    return res;
}

}  // namespace server::http

USERVER_NAMESPACE_END
