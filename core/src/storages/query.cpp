#include <userver/storages/query.hpp>

#include <userver/tracing/span.hpp>
#include <userver/tracing/tags.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages {

std::optional<Query::NameView> Query::GetNameView() const noexcept {
    return dynamic_.name_ ? std::optional<Query::NameView>{dynamic_.name_->GetUnderlying()} : std::nullopt;
}

utils::zstring_view Query::GetStatementView() const noexcept { return utils::zstring_view{dynamic_.statement_}; }

void Query::FillSpanTags(tracing::Span& span) const {
    switch (log_mode_) {
        case LogMode::kFull:
            span.AddTag(tracing::kDatabaseStatement, std::string{GetStatementView()});
            [[fallthrough]];
        case LogMode::kNameOnly:
            const auto name = GetNameView();
            if (name) {
                span.AddTag(tracing::kDatabaseStatementName, std::string{*name});
            }
    }
}

}  // namespace storages

USERVER_NAMESPACE_END
