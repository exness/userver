#include <userver/ugrpc/client/impl/client_data.hpp>

#include <userver/utils/assert.hpp>

#include <userver/ugrpc/client/client_qos.hpp>
#include <userver/ugrpc/client/impl/completion_queue_pool.hpp>
#include <userver/ugrpc/impl/statistics_storage.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

ClientData::~ClientData() { config_subscription_.Unsubscribe(); }

grpc::CompletionQueue& ClientData::NextQueue() const { return internals_.completion_queues.NextQueue(); }

ugrpc::impl::MethodStatistics& ClientData::GetStatistics(std::size_t method_id) const {
    UASSERT(service_statistics_);
    return service_statistics_->GetMethodStatistics(method_id);
}

ugrpc::impl::MethodStatistics& ClientData::GetGenericStatistics(std::string_view call_name) const {
    return internals_.statistics_storage.GetGenericStatistics(call_name, internals_.client_name);
}

const ugrpc::impl::StaticServiceMetadata& ClientData::GetMetadata() const {
    UASSERT(metadata_);
    return *metadata_;
}

const dynamic_config::Key<ClientQos>* ClientData::GetClientQos() const { return internals_.qos; }

ugrpc::impl::ServiceStatistics& ClientData::GetServiceStatistics() {
    return internals_.statistics_storage.GetServiceStatistics(GetMetadata(), internals_.client_name);
}

ChannelFactory ClientData::CreateChannelFactory(const ClientInternals& internals) {
    auto credentials = internals.testsuite_grpc.IsTlsEnabled()
                           ? GetClientCredentials(internals.client_factory_settings, internals.client_name)
                           : grpc::InsecureChannelCredentials();
    return ChannelFactory{
        internals.channel_task_processor,
        internals.endpoint,
        std::move(credentials),
        internals.client_factory_settings.channel_args};
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
