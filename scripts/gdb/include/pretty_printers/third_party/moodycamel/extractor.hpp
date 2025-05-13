#pragma once

#include <moodycamel/concurrentqueue.h>  // Y_IGNORE
#include <vector>

namespace moodycamel_extractor {

template <size_t Size, size_t Align>
struct TypeShadow {
    alignas(Align) char data[Size];
};

template <size_t Size, size_t Align>
struct AddressCatcher : TypeShadow<Size, Align> {
    AddressCatcher() = default;
    AddressCatcher(const AddressCatcher&) = default;
    AddressCatcher& operator=(const AddressCatcher&) = default;

    struct CatcherInterruption : std::exception {};

    ~AddressCatcher() noexcept(false) {
        addresses.push_back(this);
        if (destroys_until_throw == 0) {
            throw CatcherInterruption();
        }
        --destroys_until_throw;
    }

    __attribute__((noinline)) static void reset_addresses() { addresses.clear(); }

    static size_t destroys_until_throw;
    static std::vector<void*> addresses;
};

template <size_t Size, size_t Align>
size_t AddressCatcher<Size, Align>::destroys_until_throw;

template <size_t Size, size_t Align>
std::vector<void*> AddressCatcher<Size, Align>::addresses;

template <typename Catcher>
void queue_of_catchers_toucher() {
    Catcher::addresses = {};
    Catcher::destroys_until_throw = 100;
    {
        moodycamel::ConcurrentQueue<Catcher> queue;
        queue.enqueue(Catcher());
        Catcher result;
        queue.try_dequeue(result);
    }
    Catcher::addresses = {};
    Catcher::destroys_until_throw = 0;
}

template <typename T>
void instantiate(const moodycamel::ConcurrentQueue<T>&) {
    // Just template instantiation
    using Catcher = AddressCatcher<sizeof(T), alignof(T)>;
    queue_of_catchers_toucher<Catcher>();
    Catcher::reset_addresses();
}

}  // namespace moodycamel_extractor
