#include <moodycamel/concurrentqueue.h>  // Y_IGNORE
#include <userver/engine/async.hpp>
#include <userver/engine/run_standalone.hpp>
#include <userver/engine/wait_all_checked.hpp>
#include <userver/gdb_tests/stub.hpp>

#include <pretty_printers/third_party/moodycamel/extractor.hpp>

__attribute__((optnone)) static void test_queue_simple() {
    {
        moodycamel::ConcurrentQueue<int> queue;
        moodycamel_extractor::instantiate(queue);
        TEST_INIT(queue);

        queue.enqueue(1);
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue = {1}');

        queue.enqueue(2);
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue = {1, 2}');

        int item = 0;
        queue.try_dequeue(item);
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue = {2}');

        queue.try_dequeue(item);
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue');

        DoNotOptimize(queue);
        TEST_DEINIT(queue);
    }
    {
        moodycamel::ConcurrentQueue<std::string> queue;
        moodycamel_extractor::instantiate(queue);
        TEST_INIT(queue);

        queue.enqueue("hello");
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue = {"hello"}');

        queue.enqueue("world");
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue = {"hello", "world"}');

        std::string item;
        queue.try_dequeue(item);
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue = {"world"}');

        queue.try_dequeue(item);
        TEST_EXPR('queue', 'moodycamel::ConcurrentQueue');

        DoNotOptimize(queue);
        TEST_DEINIT(queue);
    }
}

template <typename T>
struct TemplateStruct {
    TemplateStruct() = default;

    T data = T();
};

template <typename T, typename U>
struct Template2Struct {
    Template2Struct() = default;

    T data1 = T();
    U data2 = U();
};

template <typename T>
struct Complex1 {
    T a;
    std::string b;
    TemplateStruct<TemplateStruct<TemplateStruct<TemplateStruct<TemplateStruct<TemplateStruct<T>>>>>> deep;
};

template <typename T>
struct Complex2 {
    Template2Struct<Template2Struct<TemplateStruct<int>, float>, Template2Struct<Template2Struct<double, int>, bool>>
        data;
};

__attribute__((optnone)) static void test_queue_complex_structure() {
    {
        moodycamel::ConcurrentQueue<Complex1<float>> queue;
        moodycamel_extractor::instantiate(queue);
        TEST_INIT(queue);

        queue.enqueue(Complex1<float>{3.14, "hello", {}});
        TEST_EXPR(
            'queue',
            'moodycamel::ConcurrentQueue = {{a = 3.1400001, b = "hello", deep = {data = {data = {data = {data = {data = {data = 0}}}}}}}}'
        );

        queue.enqueue(Complex1<float>{1.23, "very very very very big world", {}});
        TEST_EXPR(
            'queue',
            'moodycamel::ConcurrentQueue = {{a = 3.1400001, b = "hello", deep = {data = {data = {data = {data = {data = {data = 0}}}}}}}, {a = 1.23000002, b = "very very very very big world", deep = {data = {data = {data = {data = {data = {data = 0}}}}}}}}'
        );

        DoNotOptimize(queue);
        TEST_DEINIT(queue);
    }
    {
        moodycamel::ConcurrentQueue<Complex2<float>> queue;
        moodycamel_extractor::instantiate(queue);
        TEST_INIT(queue);

        queue.enqueue(Complex2<float>{});
        TEST_EXPR(
            'queue',
            'moodycamel::ConcurrentQueue = {{data = {data1 = {data1 = {data = 0}, data2 = 0}, data2 = {data1 = {data1 = 0, data2 = 0}, data2 = false}}}}'
        );

        DoNotOptimize(queue);
        TEST_DEINIT(queue);
    }
}

USERVER_NAMESPACE_BEGIN

__attribute__((optnone)) static void test_queue_stress() {
    moodycamel::ConcurrentQueue<int> queue;
    moodycamel_extractor::instantiate(queue);
    TEST_INIT(queue);

    engine::RunStandalone(10, [&] {
        std::vector<engine::TaskWithResult<void>> tasks;
        for (size_t i = 0; i < 10; ++i) {
            tasks.emplace_back(engine::AsyncNoSpan([&] {
                for (size_t i = 0; i < 10; ++i) {
                    queue.enqueue(1);
                }
            }));
        }
        engine::WaitAllChecked(tasks);
    });

    TEST_EXPR(
        'queue',
        'moodycamel::ConcurrentQueue = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}'
    );

    engine::RunStandalone(10, [&] {
        std::vector<engine::TaskWithResult<void>> tasks;
        for (size_t i = 0; i < 10; ++i) {
            tasks.emplace_back(engine::AsyncNoSpan([&] {
                int item = 0;
                while (queue.try_dequeue(item))
                    ;
            }));
        }
        engine::WaitAllChecked(tasks);
    });

    TEST_EXPR('queue', 'moodycamel::ConcurrentQueue');

    DoNotOptimize(queue);
    TEST_DEINIT(queue);
}

USERVER_NAMESPACE_END

static void test_gdb_printers() {
    test_queue_simple();
    test_queue_complex_structure();
    USERVER_NAMESPACE::test_queue_stress();
}

int main() { test_gdb_printers(); }
