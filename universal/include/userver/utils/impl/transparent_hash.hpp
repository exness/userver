#pragma once

#include <functional>
#include <string_view>

#if defined(USERVER_IMPL_ORIGINAL_CXX_STANDARD)

#if USERVER_IMPL_ORIGINAL_CXX_STANDARD <= 17
#define USERVER_IMPL_TRANSPARENT_HASH_LEGACY
#endif

#elif __cpp_lib_generic_unordered_lookup < 201811L
#define USERVER_IMPL_TRANSPARENT_HASH_LEGACY
#endif

#ifdef USERVER_IMPL_TRANSPARENT_HASH_LEGACY
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#else
#include <unordered_map>
#include <unordered_set>
#endif

USERVER_NAMESPACE_BEGIN

namespace utils::impl {

template <typename Key>
struct TransparentHash : public std::hash<std::string_view> {
    static_assert(
        std::is_convertible_v<Key, std::string_view>,
        "TransparentHash is only implemented for strings for far"
    );

    using is_transparent [[maybe_unused]] = void;
};

// Use:
// - std::unordered_{map,set} in C++20
// - boost::unordered_{map,set} in C++17

#ifndef USERVER_IMPL_TRANSPARENT_HASH_LEGACY
template <
    typename Key,
    typename Value,
    typename Hash = TransparentHash<Key>,
    typename Equal = std::equal_to<>,
    typename Allocator = std::allocator<std::pair<const Key, Value>>>
using TransparentMap = std::unordered_map<Key, Value, Hash, Equal, Allocator>;

template <
    typename Key,
    typename Hash = TransparentHash<Key>,
    typename Equal = std::equal_to<>,
    typename Allocator = std::allocator<Key>>
using TransparentSet = std::unordered_set<Key, Hash, Equal, Allocator>;
#else
template <
    typename Key,
    typename Value,
    typename Hash = TransparentHash<Key>,
    typename Equal = std::equal_to<>,
    typename Allocator = std::allocator<std::pair<const Key, Value>>>
using TransparentMap = boost::unordered_map<Key, Value, Hash, Equal, Allocator>;

template <
    typename Key,
    typename Hash = TransparentHash<Key>,
    typename Equal = std::equal_to<>,
    typename Allocator = std::allocator<Key>>
using TransparentSet = boost::unordered_set<Key, Hash, Equal, Allocator>;
#endif

template <typename TransparentContainer, typename Key>
auto FindTransparent(TransparentContainer&& container, const Key& key) {
    static_assert(!std::is_rvalue_reference_v<TransparentContainer>, "Dangling");
#ifndef USERVER_IMPL_TRANSPARENT_HASH_LEGACY
    return container.find(key);
#else
    return container.find(key, container.hash_function(), container.key_eq());
#endif
}

template <typename TransparentMap, typename Key>
auto* FindTransparentOrNullptr(TransparentMap&& map, const Key& key) {
    static_assert(!std::is_rvalue_reference_v<TransparentMap>, "Dangling");
    const auto iterator = FindTransparent(map, key);
    return iterator == map.end() ? nullptr : &iterator->second;
}

template <typename TransparentMap, typename Key, typename Value>
void TransparentInsertOrAssign(TransparentMap& map, Key&& key, Value&& value) {
    using StoredKey = typename TransparentMap::key_type;
    using ForwardedKey = std::conditional_t<std::is_same_v<std::decay_t<Key>, StoredKey>, Key&&, StoredKey>;
#ifndef USERVER_IMPL_TRANSPARENT_HASH_LEGACY
    // Still no heterogeneous support in insert_or_assign - this will result in
    // an extra copy of 'key' if 'key' is already present. See wg21.link/P2363.
    map.insert_or_assign(static_cast<ForwardedKey>(key), std::forward<Value>(value));
#else
    const auto iterator = map.find(key, map.hash_function(), map.key_eq());
    if (iterator != map.end()) {
        iterator->second = std::forward<Value>(value);
    } else {
        // Performs an extra lookup. Oh well, Boost has no insert_or_assign support.
        map.emplace(static_cast<ForwardedKey>(key), std::forward<Value>(value));
    }
#endif
}

}  // namespace utils::impl

USERVER_NAMESPACE_END
