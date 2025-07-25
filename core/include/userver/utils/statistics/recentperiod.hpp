#pragma once

#include <atomic>
#include <chrono>
#include <type_traits>

#include <userver/utils/datetime.hpp>
#include <userver/utils/fixed_array.hpp>
#include <userver/utils/statistics/fwd.hpp>
#include <userver/utils/statistics/recentperiod_detail.hpp>

USERVER_NAMESPACE_BEGIN

namespace utils::statistics {

/** \brief Class maintains circular buffer of Counters
 *
 * At any time current Counter is accessible for modification via
 * GetCurrentCounter().
 * Counter can provide a Reset() member function to clear contents.
 * @see utils::statistics::Percentile
 */
template <typename Counter, typename Result, typename Timer = utils::datetime::SteadyClock>
class RecentPeriod {
public:
    using Duration = typename Timer::duration;

    static_assert(
        (detail::kResultWantsAddFunction<Result, Counter, Duration> || detail::kResultCanUseAddAssign<Result, Counter>),
        "The Result template type argument must provide either Add(Counter, "
        "Duration, Duration) function or add assignment operator"
    );

    static constexpr bool kUseAddFunction = detail::kResultWantsAddFunction<Result, Counter, Duration>;

    /**
     * @param epoch_duration duration of epoch.
     * @param max_duration max duration to calculate statistics for
     *        must be multiple of epoch_duration.
     */
    RecentPeriod(Duration epoch_duration = std::chrono::seconds(5), Duration max_duration = std::chrono::seconds(60))
        : epoch_duration_(epoch_duration),
          max_duration_(max_duration),
          epoch_index_(0),
          items_(GetSizeForDuration(epoch_duration, max_duration)) {}

    Counter& GetCurrentCounter() { return items_[GetCurrentIndex()].counter; }

    Counter& GetPreviousCounter(int epochs_ago) { return items_[GetPreviousIndex(epochs_ago)].counter; }

    /** \brief Aggregates counters within given time range
     *
     * @param duration Time range. Special value Duration::min() -> use
     *        whole RecentPeriod range.
     * @param with_current_epoch  Include current (possibly unfinished) counter
     *        into aggregation
     *
     * Type Result must have method Add(Counter, Duration, Duration) or allow
     * addition of counter values
     */
    // NOLINTNEXTLINE(readability-const-return-type)
    const Result GetStatsForPeriod(Duration duration = Duration::min(), bool with_current_epoch = false) const {
        if (duration == Duration::min()) {
            duration = max_duration_;
        }

        Result result{};
        const Duration now = Timer::now().time_since_epoch();
        const Duration current_epoch = GetEpochForDuration(now);
        const Duration start_epoch = current_epoch - duration;
        const Duration first_epoch_duration = now - current_epoch;
        std::size_t index = epoch_index_.load();

        for (std::size_t i = 0; i < items_.size(); i++, index = (index + items_.size() - 1) % items_.size()) {
            const Duration epoch = items_[index].epoch;

            if (epoch > current_epoch) continue;
            if (epoch == current_epoch && !with_current_epoch) continue;
            if (epoch < start_epoch) break;

            if constexpr (kUseAddFunction) {
                const Duration this_epoch_duration = (i == 0) ? first_epoch_duration : epoch_duration_;

                const Duration before_this_epoch_duration = epoch - start_epoch;
                result.Add(items_[index].counter, this_epoch_duration, before_this_epoch_duration);
            } else {
                result += items_[index].counter;
            }
        }

        return result;
    }

    Duration GetEpochDuration() const { return epoch_duration_; }

    Duration GetMaxDuration() const { return max_duration_; }

    void UpdateEpochIfOld() { [[maybe_unused]] auto ignore = GetCurrentIndex(); }

    void Reset() {
        for (auto& item : items_) {
            item.Reset();
        }
    }

private:
    size_t GetCurrentIndex() const {
        while (true) {
            const Duration now = Timer::now().time_since_epoch();
            const Duration epoch = GetEpochForDuration(now);
            std::size_t index = epoch_index_.load();
            const Duration bucket_epoch = items_[index].epoch.load();

            if (epoch != bucket_epoch) {
                const std::size_t new_index = (index + 1) % items_.size();

                if (epoch_index_.compare_exchange_weak(index, new_index)) {
                    items_[new_index].epoch = epoch;
                    items_[(new_index + 1) % items_.size()].Reset();
                    return new_index;
                }
            } else {
                return index;
            }
        }
    }

    std::size_t GetPreviousIndex(int epochs_ago) {
        int index = static_cast<int>(GetCurrentIndex()) - epochs_ago;
        while (index < 0) index += items_.size();
        return index % items_.size();
    }

    Duration GetEpochForDuration(Duration duration) const {
        auto now = std::chrono::duration_cast<Duration>(duration);
        return now - now % epoch_duration_;
    }

    static std::size_t GetSizeForDuration(Duration epoch_duration, Duration max_duration) {
        /* 3 = current bucket, next zero bucket and extra one to handle
           possible race. */
        return max_duration.count() / epoch_duration.count() + 3;
    }

    struct EpochBucket {
        static constexpr bool kUseReset = detail::kCanReset<Counter>;
        std::atomic<Duration> epoch;
        Counter counter;

        EpochBucket() { Reset(); }

        void Reset() {
            epoch = Duration::min();
            if constexpr (kUseReset) {
                counter.Reset();
            } else {
                counter = 0;
            }
        }
    };

    const Duration epoch_duration_;
    const Duration max_duration_;
    mutable std::atomic_size_t epoch_index_;
    mutable utils::FixedArray<EpochBucket> items_;
};

/// @a Writer support for @a RecentPeriod
template <typename Counter, typename Result, typename Timer>
void DumpMetric(Writer& writer, const RecentPeriod<Counter, Result, Timer>& recent_period) {
    writer = recent_period.GetStatsForPeriod();
}

/// Reset support for @a RecentPeriod
template <typename Counter, typename Result, typename Timer>
void ResetMetric(RecentPeriod<Counter, Result, Timer>& recent_period) {
    recent_period.Reset();
}

}  // namespace utils::statistics

USERVER_NAMESPACE_END
