#pragma once

/// @file userver/components/component_context.hpp
/// @brief @copybrief components::ComponentContext

#include <stdexcept>
#include <string_view>

#include <userver/compiler/demangle.hpp>
#include <userver/components/component_fwd.hpp>
#include <userver/components/raw_component_base.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/utils/impl/internal_tag.hpp>

// TODO remove extra includes
#include <functional>
#include <memory>
#include <string>
#include <vector>

USERVER_NAMESPACE_BEGIN

namespace engine::impl {
class TaskContext;
}  // namespace engine::impl

namespace components {

namespace impl {

class Manager;
class ComponentContextImpl;
class ComponentInfo;

template <class T>
constexpr auto NameFromComponentType() -> decltype(std::string_view{T::kName}) {
    return T::kName;
}

template <class T, class... Args>
constexpr auto NameFromComponentType(Args...) {
    static_assert(
        !sizeof(T),
        "Component does not have a 'kName' member convertible to "
        "std::string_view. You have to explicitly specify the name: "
        "context.FindComponent<T>(name) or "
        "context.FindComponentOptional<T>(name)."
    );
    return std::string_view{};
}

}  // namespace impl

/// @brief Exception that is thrown from
/// components::ComponentContext::FindComponent() if a component load failed.
class ComponentsLoadCancelledException : public std::runtime_error {
public:
    ComponentsLoadCancelledException();
    explicit ComponentsLoadCancelledException(std::string_view message);
};

/// @brief Class to retrieve other components.
///
/// Only the const member functions of this class are meant for usage in
/// component constructor (because of that this class is always passed as a
/// const reference to the constructors).
///
/// @warning Don't store references to `ComponentContext` in your component!
/// Lifetime of the passed `ComponentContext` ends as soon as
/// the constructor ends.
///
/// For usage outside the component constructor see components::State.
///
/// @see @ref userver_components
class ComponentContext final {
public:
    ComponentContext(ComponentContext&&) = delete;
    ComponentContext& operator=(ComponentContext&&) = delete;

    /// @brief Finds a component of type T with specified name (if any) and
    /// returns the component after it was initialized.
    ///
    /// Can only be called from other component's constructor in a task where
    /// that constructor was called.
    /// May block and asynchronously wait for the creation of the requested
    /// component.
    /// @throw ComponentsLoadCancelledException if components loading was
    /// cancelled due to errors in the creation of other component.
    /// @throw std::runtime_error if component missing in `component_list` was
    /// requested.
    template <typename T>
    T& FindComponent() const {
        return FindComponent<T>(impl::NameFromComponentType<T>());
    }

    /// @overload T& FindComponent()
    template <typename T>
    T& FindComponent(std::string_view name) const {
        if (!Contains(name)) {
            ThrowNonRegisteredComponent(name, compiler::GetTypeName<T>());
        }

        auto* component_base = DoFindComponent(name);
        T* ptr = dynamic_cast<T*>(component_base);
        if (!ptr) {
            ThrowComponentTypeMismatch(name, compiler::GetTypeName<T>(), component_base);
        }

        return *ptr;
    }

    template <typename T>
    T& FindComponent(std::string_view /*name*/ = {}) {
        return ReportMisuse<T>();
    }

    /// @brief If there's no component with specified type and name return
    /// nullptr; otherwise behaves as FindComponent().
    template <typename T>
    T* FindComponentOptional() const {
        return FindComponentOptional<T>(impl::NameFromComponentType<T>());
    }

    /// @overload T* FindComponentOptional()
    template <typename T>
    T* FindComponentOptional(std::string_view name) const {
        if (!Contains(name)) {
            return nullptr;
        }
        return dynamic_cast<T*>(DoFindComponent(name));
    }

    template <typename T>
    T& FindComponentOptional(std::string_view /*name*/ = {}) {
        return ReportMisuse<T>();
    }

    /// @brief Returns an engine::TaskProcessor with the specified name.
    engine::TaskProcessor& GetTaskProcessor(std::string_view name) const;

    template <typename T>
    engine::TaskProcessor& GetTaskProcessor(const T&) {
        return ReportMisuse<T>();
    }

    /// @brief Returns the current component name. This is helpful in cases
    /// where multiple instances of the component class may be created using
    /// `component_list.Append<T>("custom-name")` syntax.
    ///
    /// @warning The lifetime of the returned string ends as soon as
    /// the current component's constructor completes. Store it
    /// as an `std::string` if needed.
    std::string_view GetComponentName() const;

    /// @cond
    // For internal use only.
    ComponentContext(
        utils::impl::InternalTag,
        impl::ComponentContextImpl& impl,
        impl::ComponentInfo& component_info
    ) noexcept;

    // For internal use only.
    impl::ComponentContextImpl& GetImpl(utils::impl::InternalTag) const;

    // For internal use only.
    const impl::Manager& GetManager(utils::impl::InternalTag) const;
    /// @endcond

private:
    /// @returns true if there is a component with the specified name and it
    /// could be found via FindComponent()
    bool Contains(std::string_view name) const noexcept;

    template <typename T>
    bool Contains(const T&) {
        return ReportMisuse<T>();
    }

    template <class T>
    decltype(auto) ReportMisuse() {
        static_assert(
            !sizeof(T),
            "components::ComponentContext should be accepted by "
            "a constant reference, i.e. "
            "`MyComponent(const components::ComponentConfig& config, "
            "const components::ComponentContext& context)`"
        );
        return 0;
    }

    [[noreturn]] void ThrowNonRegisteredComponent(std::string_view name, std::string_view type) const;

    [[noreturn]] void
    ThrowComponentTypeMismatch(std::string_view name, std::string_view type, RawComponentBase* component) const;

    RawComponentBase* DoFindComponent(std::string_view name) const;

    impl::ComponentContextImpl& impl_;
    impl::ComponentInfo& component_info_;
};

}  // namespace components

USERVER_NAMESPACE_END
