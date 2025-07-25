#include <userver/engine/io/tls_wrapper.hpp>

#include <boost/stacktrace/stacktrace.hpp>
#include <exception>
#include <memory>

#include <fmt/format.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <userver/crypto/openssl.hpp>
#include <userver/engine/io/exception.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>

#include <crypto/helpers.hpp>
#include <engine/io/fd_control.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine::io {
namespace {

struct SslCtxDeleter {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};
using SslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};
using Ssl = std::unique_ptr<SSL, SslDeleter>;

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free_all(bio); }
};
using Bio = std::unique_ptr<BIO, BioDeleter>;

#if OPENSSL_VERSION_NUMBER >= 0x010100000L
struct BioMethodDeleter {
    void operator()(BIO_METHOD* biom) { BIO_meth_free(biom); }
};
using BioMethod = std::unique_ptr<BIO_METHOD, BioMethodDeleter>;
#else
void* BIO_get_data(BIO* bio) { return bio->ptr; }
void BIO_set_data(BIO* bio, void* data) { bio->ptr = data; }
void BIO_set_init(BIO* bio, int init) { bio->init = init; }
void BIO_set_shutdown(BIO* bio, int shutdown) { bio->shutdown = shutdown; }
#endif

constexpr const char* kBioMethodName = "userver-socket";

struct SocketBioData {
    explicit SocketBioData(Socket&& socket) : socket(std::move(socket)) {
        if (!this->socket) {
            throw TlsException("Cannot use an invalid socket for TLS");
        }
    }

    Socket socket;
    Deadline current_deadline;
    std::exception_ptr last_exception;
};

int SocketBioWriteEx(BIO* bio, const char* data, size_t len, size_t* bytes_written) noexcept {
    auto* bio_data = static_cast<SocketBioData*>(BIO_get_data(bio));
    UASSERT(bio_data);
    UASSERT(bytes_written);

    try {
        *bytes_written = bio_data->socket.SendAll(data, len, bio_data->current_deadline);
        BIO_clear_retry_flags(bio);
        if (bio_data->last_exception) bio_data->last_exception = {};
        if (*bytes_written) return 1;  // success
    } catch (const engine::io::IoInterrupted& ex) {
        *bytes_written = ex.BytesTransferred();
        BIO_set_retry_write(bio);
        bio_data->last_exception = std::current_exception();
    } catch (...) {
        bio_data->last_exception = std::current_exception();
    }
    return 0;
}

int SocketBioReadEx(BIO* bio, char* data, size_t len, size_t* bytes_read) noexcept {
    auto* bio_data = static_cast<SocketBioData*>(BIO_get_data(bio));
    UASSERT(bio_data);
    UASSERT(bytes_read);

    try {
        *bytes_read = bio_data->socket.RecvSome(data, len, bio_data->current_deadline);
        BIO_clear_retry_flags(bio);
        if (bio_data->last_exception) bio_data->last_exception = {};
        if (*bytes_read) return 1;  // success
    } catch (const engine::io::IoInterrupted&) {
        BIO_set_retry_read(bio);
        bio_data->last_exception = std::current_exception();
    } catch (...) {
        bio_data->last_exception = std::current_exception();
    }
    return 0;
}

long SocketBioControl(BIO*, int cmd, long, void*) noexcept {
    if (cmd == BIO_CTRL_FLUSH) {
        // ignore for Socket
        return 1;
    }
    return 0;
}

#if OPENSSL_VERSION_NUMBER >= 0x010100000L
int SocketBioCreate(BIO* bio) noexcept {
    UASSERT(bio);
    return 1;
}

const BIO_METHOD* GetSocketBioMethod() {
    static const auto kMethod = []() -> BioMethod {
        BioMethod method{BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, kBioMethodName)};
        if (!method) return {};
        if (1 != BIO_meth_set_write_ex(method.get(), &SocketBioWriteEx)) return {};
        if (1 != BIO_meth_set_read_ex(method.get(), &SocketBioReadEx)) return {};
        if (1 != BIO_meth_set_ctrl(method.get(), &SocketBioControl)) return {};
        // should be defined to prevent setting bio->init in BIO_new
        if (1 != BIO_meth_set_create(method.get(), &SocketBioCreate)) return {};
        return method;
    }();
    return kMethod.get();
}
#else
int SocketBioRead(BIO* bio, char* data, int len) {
    UASSERT(len >= 0);
    size_t bytes_read = 0;
    if (1 != SocketBioReadEx(bio, data, len, &bytes_read)) {
        return -1;
    }
    return bytes_read;
}

int SocketBioWrite(BIO* bio, const char* data, int len) {
    UASSERT(len >= 0);
    size_t bytes_written = 0;
    if (1 != SocketBioWriteEx(bio, data, len, &bytes_written)) {
        return -1;
    }
    return bytes_written;
}

BIO_METHOD* GetSocketBioMethod() {
    static const auto kMethod = []() -> BIO_METHOD {
        BIO_METHOD method{};
        method.type = BIO_TYPE_SOURCE_SINK;
        method.name = kBioMethodName;
        method.bread = &SocketBioRead;
        method.bwrite = &SocketBioWrite;
        method.ctrl = &SocketBioControl;
        return method;
    }();
    // not actually modified, required by openssl1.0 BIO_new
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<BIO_METHOD*>(&kMethod);
}

int SSL_read_ex(SSL* ssl, void* data, size_t len, size_t* bytes_read) {
    auto ret = SSL_read(ssl, data, len);
    if (ret > 0) {
        *bytes_read = ret;
        return 1;
    }
    return ret;  // can return -1 but it's required for error processing in 1.0
}

int SSL_peek_ex(SSL* ssl, void* data, size_t len, size_t* bytes_read) {
    auto ret = SSL_peek(ssl, data, len);
    if (ret > 0) {
        *bytes_read = ret;
        return 1;
    }
    return ret;  // can return -1 but it's required for error processing in 1.0
}

int SSL_write_ex(SSL* ssl, const void* data, size_t len, size_t* bytes_written) {
    auto ret = SSL_write(ssl, data, len);
    if (ret > 0) {
        *bytes_written = ret;
        return 1;
    }
    return ret;  // can return -1 but it's required for error processing in 1.0
}
#endif

SslCtx MakeSslCtx() {
    crypto::Openssl::Init();

    SslCtx ssl_ctx{SSL_CTX_new(SSLv23_method())};
    if (!ssl_ctx) {
        throw TlsException(crypto::FormatSslError("Failed create an SSL context: SSL_CTX_new"));
    }
#if OPENSSL_VERSION_NUMBER >= 0x010100000L
    if (1 != SSL_CTX_set_min_proto_version(ssl_ctx.get(), TLS1_VERSION)) {
        throw TlsException(crypto::FormatSslError("Failed create an SSL context: SSL_CTX_set_min_proto_version"));
    }
#endif

    constexpr auto options = SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION
#if OPENSSL_VERSION_NUMBER >= 0x010100000L
                             | SSL_OP_NO_RENEGOTIATION
#endif
        ;
    SSL_CTX_set_options(ssl_ctx.get(), options);
    SSL_CTX_set_mode(ssl_ctx.get(), SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_clear_mode(ssl_ctx.get(), SSL_MODE_AUTO_RETRY);
    if (1 != SSL_CTX_set_default_verify_paths(ssl_ctx.get())) {
        LOG_LIMITED_WARNING() << crypto::FormatSslError("Failed create an SSL context: SSL_CTX_set_default_verify_paths"
        );
    }
    return ssl_ctx;
}

enum InterruptAction {
    kPass,
    kFail,
};

void SetServerName(SslCtx& ctx, std::string_view server_name) {
    if (server_name.empty()) {
        return;
    }

    X509_VERIFY_PARAM* verify_param = SSL_CTX_get0_param(ctx.get());
    if (!verify_param) {
        throw TlsException("Failed to set up client TLS wrapper: SSL_CTX_get0_param");
    }
    if (1 != X509_VERIFY_PARAM_set1_host(verify_param, server_name.data(), server_name.size())) {
        throw TlsException(crypto::FormatSslError("Failed to set up client TLS wrapper: X509_VERIFY_PARAM_set1_host"));
    }
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
}

void AddCertAuthorities(SslCtx& ctx, const std::vector<crypto::Certificate>& cert_authorities) {
    UASSERT(!cert_authorities.empty());
    auto* store = SSL_CTX_get_cert_store(ctx.get());
    UASSERT(store);
    for (const auto& ca : cert_authorities) {
        if (1 != X509_STORE_add_cert(store, ca.GetNative())) {
            throw TlsException(crypto::FormatSslError("Failed to set up client TLS wrapper: X509_STORE_add_cert"));
        }
    }
}

}  // namespace

class TlsWrapper::ReadContextAccessor final : public engine::impl::ContextAccessor {
public:
    explicit ReadContextAccessor(TlsWrapper::Impl& impl);

    bool IsReady() const noexcept override;

    engine::impl::EarlyWakeup TryAppendWaiter(engine::impl::TaskContext& waiter) override;

    void RemoveWaiter(engine::impl::TaskContext& waiter) noexcept override;

    void AfterWait() noexcept override;

    void RethrowErrorResult() const override;

    engine::impl::ContextAccessor& GetSocketContextAccessor() const noexcept;

    TlsWrapper::Impl& impl_;
};

class TlsWrapper::Impl {
public:
    explicit Impl(Socket&& socket) : bio_data(std::move(socket)), read_accessor(*this) {}

    Impl(Impl&& other) noexcept
        : bio_data(std::move(other.bio_data)),
          ssl(std::move(other.ssl)),
          read_accessor(*this),
          is_in_shutdown(other.is_in_shutdown) {
        UASSERT(ssl);
        UASSERT(SSL_get_rbio(ssl.get()) == SSL_get_wbio(ssl.get()));
        SyncBioData(SSL_get_rbio(ssl.get()), &other.bio_data);
    }

    void SetUp(SslCtx&& ssl_ctx) {
        Bio socket_bio{BIO_new(GetSocketBioMethod())};
        if (!socket_bio) {
            throw TlsException(crypto::FormatSslError("Failed to set up TLS wrapper: BIO_new"));
        }
        BIO_set_shutdown(socket_bio.get(), 0);
        SyncBioData(socket_bio.get(), nullptr);
        BIO_set_init(socket_bio.get(), 1);

        ssl.reset(SSL_new(ssl_ctx.get()));
        if (!ssl) {
            throw TlsException(crypto::FormatSslError("Failed to set up TLS wrapper: SSL_new"));
        }
#if OPENSSL_VERSION_NUMBER < 0x010100000L
        ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
#endif
        SSL_set_bio(ssl.get(), socket_bio.get(), socket_bio.get());
        [[maybe_unused]] const auto* disowned_bio = socket_bio.release();
    }

    void ClientConnect(const std::string& server_name, Deadline deadline) {
        if (!server_name.empty()) {
            // cast in openssl1.0 macro expansion
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            if (1 != SSL_set_tlsext_host_name(ssl.get(), server_name.c_str())) {
                throw TlsException(
                    crypto::FormatSslError("Failed to set up client TLS wrapper: SSL_set_tlsext_host_name")
                );
            }
        }

        bio_data.current_deadline = deadline;

        auto ret = SSL_connect(ssl.get());
        if (1 != ret) {
            if (bio_data.last_exception) {
                std::rethrow_exception(bio_data.last_exception);
            }

            throw TlsException(crypto::FormatSslError(
                fmt::format("Failed to set up client TLS wrapper ({})", SSL_get_error(ssl.get(), ret))
            ));
        }
    }

    template <typename SslIoFunc>
    size_t PerformSslIo(
        SslIoFunc&& io_func,
        void* buf,
        size_t len,
        impl::TransferMode mode,
        InterruptAction interrupt_action,
        Deadline deadline,
        const char* context
    ) {
        UASSERT(ssl);
        if (!len) return 0;

            /* TODO
            UASSERT_MSG(
                ssl_usage_level == 0,
                "You may not use SSL sockets concurrently from multiple coroutines");
                */
#ifndef NDEBUG
        ssl_usage_level++;
        const utils::FastScopeGuard ssl_usage_guard([this]() noexcept { --ssl_usage_level; });
#endif

        bio_data.current_deadline = deadline;

        char* const begin = static_cast<char*>(buf);
        char* const end = begin + len;
        char* pos = begin;
        while (pos < end && ssl && !(SSL_get_shutdown(ssl.get()) & SSL_RECEIVED_SHUTDOWN)) {
            size_t chunk_size = 0;
            const int io_ret = io_func(ssl.get(), pos, end - pos, &chunk_size);
            int ssl_error = SSL_ERROR_NONE;
            if (io_ret == 1) {
                pos += chunk_size;
                if (mode != impl::TransferMode::kWhole) {
                    break;
                }
            } else {
                ssl_error = SSL_get_error(ssl.get(), io_ret);
                switch (ssl_error) {
                    // timeout, cancel, EOF, or just a spurious wakeup
                    case SSL_ERROR_WANT_READ:
                    case SSL_ERROR_WANT_WRITE:
                    case SSL_ERROR_ZERO_RETURN:
                        break;

                    // connection breaking errors
                    case SSL_ERROR_SYSCALL:
                    case SSL_ERROR_SSL:
                        ssl.reset();
                        break;

                    // there should not be anything else
                    default:
                        UINVARIANT(false, fmt::format("Unexpected SSL_ERROR: {}", ssl_error));
                }
                if (bio_data.last_exception) {
                    if (interrupt_action == InterruptAction::kFail) {
                        // Sometimes (when writing) we must either retry the io_func with
                        // the same arguments or fail the channel completely. To avoid
                        // stalling, we do the latter.
                        ssl.reset();
                    }
                    std::rethrow_exception(bio_data.last_exception);
                }
                if (!ssl) {
                    // openssl breakage
                    throw TlsException(crypto::FormatSslError(std::string{context} + " failed"));
                }
            }
        }
        return pos - begin;
    }

    void CheckAlive() const {
        if (!ssl) {
            throw TlsException("SSL connection is broken");
        }
    }

    SocketBioData bio_data;
    Ssl ssl;
    ReadContextAccessor read_accessor;
    bool is_in_shutdown{false};
    std::atomic<int> ssl_usage_level{0};

private:
    void SyncBioData(BIO* bio, [[maybe_unused]] SocketBioData* old_data) noexcept {
        UASSERT(BIO_get_data(bio) == old_data);
        BIO_set_data(bio, &bio_data);
    }
};

TlsWrapper::ReadContextAccessor::ReadContextAccessor(TlsWrapper::Impl& impl) : impl_(impl) {}

bool TlsWrapper::ReadContextAccessor::IsReady() const noexcept {
    auto* ssl = impl_.ssl.get();
    if (!ssl || SSL_has_pending(ssl)) return true;
    return GetSocketContextAccessor().IsReady();
}

engine::impl::EarlyWakeup TlsWrapper::ReadContextAccessor::TryAppendWaiter(engine::impl::TaskContext& waiter) {
    auto* ssl = impl_.ssl.get();
    if (!ssl || SSL_has_pending(ssl)) return engine::impl::EarlyWakeup{true};

    return GetSocketContextAccessor().TryAppendWaiter(waiter);
}

void TlsWrapper::ReadContextAccessor::RemoveWaiter(engine::impl::TaskContext& waiter) noexcept {
    GetSocketContextAccessor().RemoveWaiter(waiter);
}

void TlsWrapper::ReadContextAccessor::AfterWait() noexcept { GetSocketContextAccessor().AfterWait(); }

void TlsWrapper::ReadContextAccessor::RethrowErrorResult() const { GetSocketContextAccessor().RethrowErrorResult(); }

engine::impl::ContextAccessor& TlsWrapper::ReadContextAccessor::GetSocketContextAccessor() const noexcept {
    auto* ca = impl_.bio_data.socket.GetReadableBase().TryGetContextAccessor();
    UASSERT(ca);
    return *ca;
}

TlsWrapper::TlsWrapper(Socket&& socket) : impl_(std::move(socket)) { SetupContextAccessors(); }

TlsWrapper TlsWrapper::StartTlsClient(Socket&& socket, const std::string& server_name, Deadline deadline) {
    auto ssl_ctx = MakeSslCtx();
    SetServerName(ssl_ctx, server_name);

    TlsWrapper wrapper{std::move(socket)};
    wrapper.impl_->SetUp(std::move(ssl_ctx));
    wrapper.impl_->ClientConnect(server_name, deadline);
    return wrapper;
}

TlsWrapper TlsWrapper::StartTlsClient(
    Socket&& socket,
    const std::string& server_name,
    const crypto::Certificate& cert,
    const crypto::PrivateKey& key,
    Deadline deadline,
    const std::vector<crypto::Certificate>& extra_cert_authorities
) {
    auto ssl_ctx = MakeSslCtx();
    SetServerName(ssl_ctx, server_name);

    if (!extra_cert_authorities.empty()) {
        AddCertAuthorities(ssl_ctx, extra_cert_authorities);
    }

    if (cert) {
        if (1 != SSL_CTX_use_certificate(ssl_ctx.get(), cert.GetNative())) {
            throw TlsException(crypto::FormatSslError("Failed to set up client TLS wrapper: SSL_CTX_use_certificate"));
        }
    }

    if (key) {
        if (1 != SSL_CTX_use_PrivateKey(ssl_ctx.get(), key.GetNative())) {
            throw TlsException(crypto::FormatSslError("Failed to set up client TLS wrapper: SSL_CTX_use_PrivateKey"));
        }
    }

    TlsWrapper wrapper{std::move(socket)};
    wrapper.impl_->SetUp(std::move(ssl_ctx));
    wrapper.impl_->ClientConnect(server_name, deadline);
    return wrapper;
}

TlsWrapper TlsWrapper::StartTlsServer(
    Socket&& socket,
    const crypto::CertificatesChain& cert_chain,
    const crypto::PrivateKey& key,
    Deadline deadline,
    const std::vector<crypto::Certificate>& extra_cert_authorities
) {
    auto ssl_ctx = MakeSslCtx();

    if (!extra_cert_authorities.empty()) {
        AddCertAuthorities(ssl_ctx, extra_cert_authorities);
        SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        LOG_INFO() << "Client SSL cert will be verified";
    } else {
        LOG_INFO() << "Client SSL cert will not be verified";
    }

    if (cert_chain.empty()) {
        throw TlsException(crypto::FormatSslError("Empty certificate chain provided"));
    }

    if (1 != SSL_CTX_use_certificate(ssl_ctx.get(), cert_chain.begin()->GetNative())) {
        throw TlsException(crypto::FormatSslError("Failed to set up server TLS wrapper: SSL_CTX_use_certificate"));
    }

    if (cert_chain.size() > 1) {
        auto cert_it = std::next(cert_chain.begin());
        for (; cert_it != cert_chain.end(); ++cert_it) {
            // cast in openssl1.0 macro expansion
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            if (SSL_CTX_add_extra_chain_cert(ssl_ctx.get(), cert_it->GetNative()) <= 0) {
                throw TlsException(
                    crypto::FormatSslError("Failed to set up server TLS wrapper: SSL_CTX_add_extra_chain_cert")
                );
            }

            // After SSL_CTX_add_extra_chain_cert we should not free the cert
            const auto ret = X509_up_ref(cert_it->GetNative());
            UASSERT(ret == 1);
        }
    }

    if (1 != SSL_CTX_use_PrivateKey(ssl_ctx.get(), key.GetNative())) {
        throw TlsException(crypto::FormatSslError("Failed to set up server TLS wrapper: SSL_CTX_use_PrivateKey"));
    }

    TlsWrapper wrapper{std::move(socket)};
    wrapper.impl_->SetUp(std::move(ssl_ctx));
    wrapper.impl_->bio_data.current_deadline = deadline;

    auto ret = SSL_accept(wrapper.impl_->ssl.get());
    if (1 != ret) {
        if (wrapper.impl_->bio_data.last_exception) {
            std::rethrow_exception(wrapper.impl_->bio_data.last_exception);
        }

        throw TlsException(crypto::FormatSslError(
            fmt::format("Failed to set up server TLS wrapper ({})", SSL_get_error(wrapper.impl_->ssl.get(), ret))
        ));
    }

    UASSERT(wrapper.impl_->ssl);
    return wrapper;
}

TlsWrapper::~TlsWrapper() {
    UASSERT(impl_->ssl_usage_level == 0);
    if (!IsValid()) return;

    // socket will not be reused, attempt unidirectional shutdown
    SSL_shutdown(impl_->ssl.get());
}

TlsWrapper::TlsWrapper(TlsWrapper&& other) noexcept : impl_(std::move(other.impl_)) { SetupContextAccessors(); }

void TlsWrapper::SetupContextAccessors() {
    // Cannot use raw Socket's accessor as some data might be already read into
    // a local buffer
    SetReadableContextAccessor(&impl_->read_accessor);

    engine::io::WritableBase& write_dir = impl_->bio_data.socket;
    SetWritableContextAccessor(write_dir.TryGetContextAccessor());
}

bool TlsWrapper::IsValid() const { return impl_->ssl && !impl_->is_in_shutdown; }

bool TlsWrapper::WaitReadable(Deadline deadline) {
    impl_->CheckAlive();
    char buf = 0;
    return impl_->PerformSslIo(
        &SSL_peek_ex, &buf, 1, impl::TransferMode::kOnce, InterruptAction::kPass, deadline, "WaitReadable"
    );
}

bool TlsWrapper::WaitWriteable(Deadline deadline) {
    impl_->CheckAlive();
    return impl_->bio_data.socket.WaitWriteable(deadline);
}

size_t TlsWrapper::RecvSome(void* buf, size_t len, Deadline deadline) {
    impl_->CheckAlive();
    return impl_->PerformSslIo(
        &SSL_read_ex, buf, len, impl::TransferMode::kOnce, InterruptAction::kPass, deadline, "RecvSome"
    );
}

size_t TlsWrapper::RecvAll(void* buf, size_t len, Deadline deadline) {
    impl_->CheckAlive();
    return impl_->PerformSslIo(
        &SSL_read_ex, buf, len, impl::TransferMode::kWhole, InterruptAction::kPass, deadline, "RecvAll"
    );
}

size_t TlsWrapper::SendAll(const void* buf, size_t len, Deadline deadline) {
    impl_->CheckAlive();
    return impl_->PerformSslIo(
        &SSL_write_ex,
        const_cast<void*>(buf),  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        len,
        impl::TransferMode::kWhole,
        InterruptAction::kFail,
        deadline,
        "SendAll"
    );
}

[[nodiscard]] size_t TlsWrapper::WriteAll(std::initializer_list<IoData> list, Deadline deadline) {
    static constexpr std::size_t kBufSize = 4'096;
    std::byte buf[kBufSize];

    std::size_t sent_bytes = 0;
    std::size_t remaining_cap = kBufSize;
    auto fits_in_buf_begin = list.begin();
    for (auto it = fits_in_buf_begin; it != list.end(); ++it) {
        if (it->len > remaining_cap) {
            if (it - fits_in_buf_begin >= 2) {
                for (auto* ins_pos = buf; fits_in_buf_begin != it; ++fits_in_buf_begin) {
                    ins_pos = std::copy_n(
                        static_cast<const std::byte*>(fits_in_buf_begin->data), fits_in_buf_begin->len, ins_pos
                    );
                }
                sent_bytes += SendAll(buf, kBufSize - remaining_cap, deadline);
            } else if (fits_in_buf_begin != it) {
                sent_bytes += SendAll(fits_in_buf_begin->data, fits_in_buf_begin->len, deadline);
                fits_in_buf_begin = it;
            }

            remaining_cap = kBufSize;
            if (it->len < remaining_cap) {
                remaining_cap -= it->len;
            } else {
                sent_bytes += SendAll(it->data, it->len, deadline);
                ++fits_in_buf_begin;
            }

        } else {
            remaining_cap -= it->len;
        }
    }

    auto ins_pos = buf;
    for (auto ins_it = fits_in_buf_begin; ins_it != list.end(); ++ins_it) {
        ins_pos = std::copy_n(static_cast<const std::byte*>(ins_it->data), ins_it->len, ins_pos);
    }
    sent_bytes += SendAll(buf, kBufSize - remaining_cap, deadline);

    return sent_bytes;
}

Socket TlsWrapper::StopTls(Deadline deadline) {
    if (impl_->ssl) {
        impl_->is_in_shutdown = true;
        impl_->bio_data.current_deadline = deadline;
        int shutdown_ret = 0;
        while (shutdown_ret != 1) {
            shutdown_ret = SSL_shutdown(impl_->ssl.get());
            if (shutdown_ret < 0) {
                const int ssl_error = SSL_get_error(impl_->ssl.get(), shutdown_ret);
                switch (ssl_error) {
                    // this is fine
                    case SSL_ERROR_WANT_READ:
                    case SSL_ERROR_WANT_WRITE:
                        break;

                    // connection breaking errors
                    case SSL_ERROR_SYSCALL:  // EOF if we didn't throw, see BUGS in man
                    case SSL_ERROR_SSL:      // protocol error, socket is in unknown state
                        impl_->bio_data.socket.Close();
                        shutdown_ret = 1;
                        break;

                    // there should not be anything else
                    default:
                        UINVARIANT(false, fmt::format("Unexpected SSL_ERROR: {}", ssl_error));
                }
                if (impl_->bio_data.last_exception) {
                    std::rethrow_exception(impl_->bio_data.last_exception);
                }
            }
        }
        impl_->ssl.reset();
    }
    return std::move(impl_->bio_data.socket);
}

int TlsWrapper::GetRawFd() { return impl_->bio_data.socket.Fd(); }

}  // namespace engine::io

USERVER_NAMESPACE_END
