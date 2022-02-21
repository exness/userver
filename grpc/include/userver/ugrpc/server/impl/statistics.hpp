#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <grpcpp/support/status.h>

#include <userver/formats/json_fwd.hpp>
#include <userver/utils/fixed_array.hpp>
#include <userver/utils/statistics/fwd.hpp>
#include <userver/utils/statistics/percentile.hpp>
#include <userver/utils/statistics/recentperiod.hpp>
#include <userver/utils/statistics/relaxed_counter.hpp>

#include <userver/ugrpc/server/impl/service_worker.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::impl {

class MethodStatistics final {
 public:
  MethodStatistics();

  void AccountStatus(grpc::StatusCode code) noexcept;

  void AccountTiming(std::chrono::milliseconds timing) noexcept;

  // All errors without gRPC status codes are categorized as "network errors".
  // See server::RpcInterruptedError.
  void AccountNetworkError() noexcept;

  // Occurs when the service forgot to finish a request, oftentimes due to a
  // thrown exception. Always indicates a programming error in our service.
  // UNKNOWN status code is automatically returned in this case.
  void AccountInternalError() noexcept;

  formats::json::Value ExtendStatistics() const;

 private:
  using Percentile =
      utils::statistics::Percentile<2000, std::uint32_t, 256, 100>;
  using Counter = utils::statistics::RelaxedCounter<std::uint64_t>;

  // StatusCode enum cases have consecutive underlying values, starting from 0.
  // UNAUTHENTICATED currently has the largest value.
  static constexpr std::size_t kCodesCount =
      static_cast<std::size_t>(grpc::StatusCode::UNAUTHENTICATED) + 1;

  std::array<Counter, kCodesCount> status_codes;
  utils::statistics::RecentPeriod<Percentile, Percentile> timings;
  Counter network_errors;
  Counter internal_errors;
};

class ServiceStatistics final {
 public:
  explicit ServiceStatistics(const StaticServiceMetadata& metadata);

  ~ServiceStatistics();

  MethodStatistics& GetMethodStatistics(std::size_t method_id);

  formats::json::Value ExtendStatistics() const;

  utils::statistics::Entry Register(
      utils::statistics::Storage& statistics_storage);

 private:
  const StaticServiceMetadata metadata_;
  utils::FixedArray<MethodStatistics> method_statistics_;
};

}  // namespace ugrpc::server::impl

USERVER_NAMESPACE_END
