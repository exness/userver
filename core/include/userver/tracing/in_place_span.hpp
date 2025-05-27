#pragma once

/// @file userver/tracing/in_place_span.hpp
/// @brief @copybrief tracing::InPlaceSpan

#include <string>

#include <userver/tracing/span.hpp>
#include <userver/utils/fast_pimpl.hpp>
#include <userver/utils/impl/internal_tag.hpp>

USERVER_NAMESPACE_BEGIN

namespace tracing {

/// @brief Avoids an extra allocation by storing tracing::Span data in-place
/// @warning Never put InPlaceSpan on the stack! It has a large size and can
/// cause stack overflow.
class InPlaceSpan final {
public:
    explicit InPlaceSpan(
        std::string&& name,
        utils::impl::SourceLocation source_location = utils::impl::SourceLocation::Current()
    );

    explicit InPlaceSpan(
        std::string&& name,
        std::string&& trace_id,
        std::string&& parent_span_id,
        utils::impl::SourceLocation source_location = utils::impl::SourceLocation::Current()
    );

    InPlaceSpan(InPlaceSpan&&) = delete;
    InPlaceSpan& operator=(InPlaceSpan&&) = delete;
    ~InPlaceSpan();

    tracing::Span& Get() noexcept;

    /// @cond
    // For internal use only.
    void SetParentLink(utils::impl::InternalTag, std::string&& parent_link);
    /// @endcond

private:
    struct Impl;
    utils::FastPimpl<Impl, 4240, 8> impl_;
};

}  // namespace tracing

USERVER_NAMESPACE_END
