#include <curl-ev/wrappers.hpp>

#include <userver/crypto/openssl.hpp>

#include <curl-ev/error_code.hpp>

USERVER_NAMESPACE_BEGIN

namespace curl::impl {

CurlGlobal::CurlGlobal() {
    crypto::Openssl::Init();
    const std::error_code ec{static_cast<errc::EasyErrorCode>(native::curl_global_init(CURL_GLOBAL_DEFAULT))};
    throw_error(ec, "cURL global initialization failed");
}

CurlGlobal::~CurlGlobal() { native::curl_global_cleanup(); }

void CurlGlobal::Init() { [[maybe_unused]] static const CurlGlobal global; }

}  // namespace curl::impl

USERVER_NAMESPACE_END
