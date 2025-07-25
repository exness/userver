#include <userver/clients/http/plugin.hpp>

#include <clients/http/request_state.hpp>
#include <userver/clients/http/request.hpp>
#include <userver/utils/algo.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {

PluginRequest::PluginRequest(RequestState& state) : state_(state) {}

void PluginRequest::SetHeader(std::string_view name, std::string_view value) {
    state_.easy().add_header(
        name, value, curl::easy::EmptyHeaderAction::kDoNotSend, curl::easy::DuplicateHeaderAction::kReplace
    );
}

void PluginRequest::AddQueryParams(std::string_view params) {
    const auto& url = state_.easy().get_original_url();
    if (url.find('?') != std::string::npos) {
        state_.easy().set_url(utils::StrCat(url, "&", params));
    } else {
        state_.easy().set_url(utils::StrCat(url, "?", params));
    }
}

void PluginRequest::SetTimeout(std::chrono::milliseconds ms) {
    state_.set_timeout(ms.count());
    state_.SetEasyTimeout(ms);
}

const std::string& PluginRequest::GetOriginalUrl() const { return state_.easy().get_original_url(); }

Plugin::Plugin(std::string name) : name_(std::move(name)) {}

const std::string& Plugin::GetName() const { return name_; }

namespace impl {

PluginPipeline::PluginPipeline(const std::vector<utils::NotNull<Plugin*>>& plugins) : plugins_(plugins) {}

void PluginPipeline::HookCreateSpan(RequestState& request_state, tracing::Span& span) {
    PluginRequest req(request_state);

    for (const auto& plugin : plugins_) {
        plugin->HookCreateSpan(req, span);
    }
}

void PluginPipeline::HookOnCompleted(RequestState& request_state, Response& response) {
    PluginRequest req(request_state);

    // NOLINTNEXTLINE(modernize-loop-convert)
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        const auto& plugin = *it;
        plugin->HookOnCompleted(req, response);
    }
}

void PluginPipeline::HookOnError(RequestState& request_state, std::error_code ec) {
    PluginRequest req(request_state);

    // NOLINTNEXTLINE(modernize-loop-convert)
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        const auto& plugin = *it;
        plugin->HookOnError(req, ec);
    }
}

bool PluginPipeline::HookOnRetry(RequestState& request_state) {
    PluginRequest req(request_state);

    for (const auto& plugin : plugins_) {
        if (!plugin->HookOnRetry(req)) return false;
    }
    return true;
}

void PluginPipeline::HookPerformRequest(RequestState& request_state) {
    PluginRequest req(request_state);

    for (const auto& plugin : plugins_) {
        plugin->HookPerformRequest(req);
    }
}

}  // namespace impl

}  // namespace clients::http

USERVER_NAMESPACE_END
