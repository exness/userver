#include <userver/utest/utest.hpp>

#include <optional>
#include <utility>

#include <userver/engine/async.hpp>
#include <userver/engine/single_use_event.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/local_variable.hpp>
#include <userver/utils/async.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

class LogStringGuard final {
public:
    LogStringGuard(std::string& destination, std::string source)
        : destination_(destination), source_(std::move(source)) {}

    ~LogStringGuard() { destination_ += source_; }

private:
    std::string& destination_;
    std::string source_;
};

engine::TaskLocalVariable<int> kIntVariable;
engine::TaskLocalVariable<std::optional<LogStringGuard>> kGuardX;
engine::TaskLocalVariable<std::optional<LogStringGuard>> kGuardY;
engine::TaskLocalVariable<std::optional<LogStringGuard>> kGuardZ;

}  // namespace

UTEST(TaskLocalVariable, SetGet) {
    EXPECT_FALSE(kIntVariable.GetOptional());

    *kIntVariable = 1;
    EXPECT_EQ(1, *kIntVariable);
    EXPECT_TRUE(kIntVariable.GetOptional());

    engine::Yield();
    EXPECT_EQ(1, *kIntVariable);

    *kIntVariable = 2;
    EXPECT_EQ(2, *kIntVariable);

    engine::Yield();
    EXPECT_EQ(2, *kIntVariable);
}

UTEST(TaskLocalVariable, TwoTask) {
    *kIntVariable = 1;

    auto task = engine::AsyncNoSpan([] {
        *kIntVariable = 2;
        EXPECT_EQ(2, *kIntVariable);

        engine::Yield();
        EXPECT_EQ(2, *kIntVariable);

        *kIntVariable = 3;
        EXPECT_EQ(3, *kIntVariable);

        engine::Yield();
        EXPECT_EQ(3, *kIntVariable);
    });

    engine::Yield();
    EXPECT_EQ(1, *kIntVariable);

    *kIntVariable = 10;
    EXPECT_EQ(10, *kIntVariable);

    engine::Yield();
    EXPECT_EQ(10, *kIntVariable);
}

UTEST(TaskLocalVariable, MultipleThreads) {
    *kIntVariable = 1;

    auto task = engine::AsyncNoSpan([] {
        *kIntVariable = 2;
        EXPECT_EQ(2, *kIntVariable);

        engine::Yield();
        EXPECT_EQ(2, *kIntVariable);

        *kIntVariable = 3;
        EXPECT_EQ(3, *kIntVariable);

        engine::Yield();
        EXPECT_EQ(3, *kIntVariable);
    });

    engine::Yield();
    EXPECT_EQ(1, *kIntVariable);

    *kIntVariable = 10;
    EXPECT_EQ(10, *kIntVariable);

    engine::Yield();
    EXPECT_EQ(10, *kIntVariable);
}

UTEST(TaskLocalVariable, Destructor) {
    std::string destruction_order;

    utils::Async("test", [&] {
        kGuardX->emplace(destruction_order, "1");
        EXPECT_EQ(destruction_order, "");

        engine::AsyncNoSpan([&] { kGuardX->emplace(destruction_order, "2"); }).Get();

        EXPECT_EQ(destruction_order, "2");
    }).Get();

    EXPECT_EQ(destruction_order, "21");
}

UTEST(TaskLocalVariable, DestructionOrder) {
    {
        std::string destruction_order;

        engine::AsyncNoSpan([&] {
            kGuardY->emplace(destruction_order, "y");
            kGuardX->emplace(destruction_order, "x");
            kGuardZ->emplace(destruction_order, "z");
        }).Get();

        // variables are destroyed in reverse-initialization order
        EXPECT_EQ(destruction_order, "zxy");
    }

    {
        std::string destruction_order;

        engine::AsyncNoSpan([&] {
            kGuardX->emplace(destruction_order, "x");
            kGuardY->emplace(destruction_order, "y");
        }).Get();

        // different tasks may have different initialization order and utilize
        // different sets of variables
        EXPECT_EQ(destruction_order, "yx");
    }
}

namespace {

class WaitingInDestructorVariable final {
public:
    explicit WaitingInDestructorVariable(engine::SingleUseEvent& event) : event_(event) {}

    ~WaitingInDestructorVariable() { event_.WaitNonCancellable(); }

private:
    engine::SingleUseEvent& event_;
};

engine::TaskLocalVariable<std::optional<WaitingInDestructorVariable>> kWaitingInDestructorVariable;

}  // namespace

UTEST(TaskLocalVariable, WaitInDestructor) {
    engine::SingleUseEvent event;
    auto task = engine::AsyncNoSpan([&] { kWaitingInDestructorVariable->emplace(event); });

    engine::SleepFor(std::chrono::milliseconds{100});
    EXPECT_FALSE(task.IsFinished());
    EXPECT_EQ(task.GetState(), engine::TaskBase::State::kSuspended);

    event.Send();
    task.Wait();
    EXPECT_EQ(task.GetState(), engine::TaskBase::State::kCompleted);
    UEXPECT_NO_THROW(task.Get());
}

UTEST(TaskLocalVariable, WaitInDestructorCancelled) {
    engine::SingleUseEvent event;
    auto task = engine::AsyncNoSpan([&] {
        kWaitingInDestructorVariable->emplace(event);
        engine::current_task::RequestCancel();
        engine::current_task::CancellationPoint();
    });

    engine::SleepFor(std::chrono::milliseconds{100});
    EXPECT_FALSE(task.IsFinished());
    EXPECT_EQ(task.GetState(), engine::TaskBase::State::kSuspended);

    event.Send();
    task.Wait();
    EXPECT_EQ(task.GetState(), engine::TaskBase::State::kCancelled);
    UEXPECT_THROW(task.Get(), engine::TaskCancelledException);
}

USERVER_NAMESPACE_END
