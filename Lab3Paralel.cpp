#include <iostream>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <shared_mutex>
#include <atomic>
#include <climits>
#include <algorithm>

using namespace std;

// Типи для зручної роботи з блокуванням читання/запису
using rw_lock_t = std::shared_mutex;
using shared_read_lock = std::shared_lock<rw_lock_t>;
using exclusive_write_lock = std::unique_lock<rw_lock_t>;

// Структура задачі з пріоритетом
struct TaskWithPriority
{
    int priority;
    function<void()> task;

    // Менший пріоритет — вища важливість
    bool operator<(const TaskWithPriority& other) const
    {
        return priority > other.priority;
    }
};

// Шаблонна черга задач з обмеженням розміру
template <typename TaskType>
class TaskQueue
{
    using PriorityQueueImpl = priority_queue<TaskType>;

public:
    TaskQueue(size_t max_capacity = 15) : max_size(max_capacity) {}
    ~TaskQueue() { clear(); }

    bool empty() const
    {
        shared_read_lock lock(task_rw_lock);
        return tasks.empty();
    }

    TaskType top() const
    {
        shared_read_lock lock(task_rw_lock);
        return tasks.top();
    }

    size_t size() const
    {
        shared_read_lock lock(task_rw_lock);
        return tasks.size();
    }

    void clear()
    {
        exclusive_write_lock lock(task_rw_lock);
        while (!tasks.empty())
        {
            tasks.pop();
        }
    }

    bool pop(TaskType& task)
    {
        exclusive_write_lock lock(task_rw_lock);
        if (tasks.empty()) return false;
        task = tasks.top();
        tasks.pop();
        return true;
    }

    bool push_if_possible(const TaskType& task)
    {
        exclusive_write_lock lock(task_rw_lock);
        if (tasks.size() < max_size)
        {
            tasks.emplace(task);
            return true;
        }
        return false;
    }

private:
    size_t max_size;
    mutable rw_lock_t task_rw_lock;
    PriorityQueueImpl tasks;
};

// Клас пулу потоків для обробки задач
class ThreadPool
{
public:
    ThreadPool() : task_queue(15) {}
    ~ThreadPool() { shutdown(); }

    // Запуск пулу з вказаною кількістю потоків
    void start(size_t num_threads)
    {
        exclusive_write_lock lock(pool_lock);
        if (initialized || stopped) return;

        threads.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i)
        {
            threads.emplace_back(&ThreadPool::worker_loop, this);
        }
        initialized = !threads.empty();
    }

    // Завершення роботи пулу
    void shutdown()
    {
        {
            exclusive_write_lock lock(pool_lock);
            stopped = true;
            force_exit = true;
        }
        task_available.notify_all();
        for (auto& t : threads)
        {
            if (t.joinable()) t.join();
        }
        threads.clear();
        initialized = false;
        stopped = false;
    }

    template <typename Func, typename... Args>
    void submit_task(int priority, Func&& func, Args&&... args)
    {
        {
            shared_read_lock lock(pool_lock);
            if (!is_running_unsafe()) return;
        }

        auto bound_task = bind(forward<Func>(func), forward<Args>(args)...);
        TaskWithPriority task{ priority, bound_task };

        bool accepted = task_queue.push_if_possible(task);

        if (accepted)
        {
            ++total_tasks_submitted;
            task_available.notify_one();

            if (task_queue.size() == 15 && !queue_full_flag.exchange(true))
            {
                full_queue_timer_start = chrono::steady_clock::now();
            }
        }
        else
        {
            ++total_tasks_rejected;

            if (queue_full_flag.exchange(false))
            {
                auto full_duration = chrono::duration_cast<chrono::milliseconds>(
                    chrono::steady_clock::now() - full_queue_timer_start).count();
                max_full_queue_duration_ms = max(max_full_queue_duration_ms.load(), full_duration);
                min_full_queue_duration_ms = min(min_full_queue_duration_ms.load(), full_duration);
            }

            lock_guard<mutex> lock(cout_lock);
            cout << "Task was rejected due to full queue (priority: " << priority << ").\n";
        }
    }

    size_t get_submitted_task_count() const { return total_tasks_submitted.load(); }
    size_t get_completed_task_count() const { return total_tasks_completed.load(); }
    size_t get_rejected_task_count() const { return total_tasks_rejected.load(); }

    double get_avg_wait_time() const
    {
        return static_cast<double>(total_waiting_time_ms.load()) / std::max(1ULL, static_cast<unsigned long long>(waiting_entries.load())) / 1000.0;
    }

    long long get_max_queue_full_time() const { return max_full_queue_duration_ms.load(); }
    long long get_min_queue_full_time() const { return (min_full_queue_duration_ms.load() == LLONG_MAX) ? 0 : min_full_queue_duration_ms.load(); }

private:
    mutable rw_lock_t pool_lock;
    mutable condition_variable_any task_available;

    vector<thread> threads;
    TaskQueue<TaskWithPriority> task_queue;

    bool initialized = false;
    bool stopped = false;
    bool force_exit = false;

    atomic<size_t> total_tasks_submitted{ 0 };
    atomic<size_t> total_tasks_completed{ 0 };
    atomic<size_t> total_tasks_rejected{ 0 };
    atomic<size_t> total_waiting_time_ms{ 0 };
    atomic<size_t> waiting_entries{ 0 };

    atomic<long long> max_full_queue_duration_ms{ 0 };
    atomic<long long> min_full_queue_duration_ms{ LLONG_MAX };
    atomic<bool> queue_full_flag{ false };
    chrono::steady_clock::time_point full_queue_timer_start;

    mutex cout_lock;

    void worker_loop()
    {
        while (true)
        {
            TaskWithPriority task;
            {
                exclusive_write_lock lock(pool_lock);

                auto wait_start = chrono::steady_clock::now();

                task_available.wait(lock, [this] {return stopped || (!task_queue.empty() || force_exit);});
                auto wait_end = chrono::steady_clock::now();
                total_waiting_time_ms += chrono::duration_cast<chrono::milliseconds>(wait_end - wait_start).count();
                ++waiting_entries;

                if (force_exit) return;

                if (!task_queue.empty())
                {
                    task_queue.pop(task);
                }
                else
                {
                    continue;
                }
            }

            if (task.task)
            {
                task.task();
                ++total_tasks_completed;
            }

            if (force_exit) return;
        }
    }

    bool is_running_unsafe() const
    {
        return initialized && !stopped;
    }
};

// Симуляція виконання задачі
void simulate_task_execution(int task_id, int seconds)
{
    {
        lock_guard<mutex> lock(cout_lock);
        cout << "Task " << task_id << " is running for " << seconds << "s.\n";
    }
    this_thread::sleep_for(chrono::seconds(seconds));
    {
        lock_guard<mutex> lock(cout_lock);
        cout << "Task " << task_id << " is done.\n";
    }
}

int get_random_duration(int min, int max)
{
    return min + rand() % (max - min + 1);
}

int get_random_interval(int min, int max)
{
    return min + rand() % (max - min + 1);
}

void stop_after_duration(int seconds)
{
    this_thread::sleep_for(chrono::seconds(seconds));
    stop_simulation = true;
}

int main()
{

    const int num_workers = 4;
    const int min_task_duration = 5;
    const int max_task_duration = 10;
    const int generation_freq = 2;
    const int num_generators = 4;
    const int simulation_seconds = 20;

    srand(static_cast<unsigned>(time(nullptr)));

    ThreadPool thread_pool;
    thread_pool.start(num_workers);

    atomic<bool> stop_task_generation(false);
    auto task_generator = [&]() {
        while (!stop_task_generation)
        {
            int duration = get_random_duration(min_task_duration, max_task_duration);
            int task_id = task_id_counter.fetch_add(1);
            thread_pool.submit_task(duration, simulate_task_execution, task_id, duration);
            int sleep_time = get_random_interval(1, 2) * generation_freq;
            this_thread::sleep_for(chrono::seconds(sleep_time));
        }
        };

    vector<thread> generators;
    for (int i = 0; i < num_generators; ++i)
    {
        generators.emplace_back(task_generator);
    }

    thread timer_thread(stop_after_duration, simulation_seconds);

    while (!stop_simulation)
    {
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    stop_task_generation = true;
    thread_pool.shutdown();

    for (auto& t : generators)
    {
        if (t.joinable()) t.join();
    }
    if (timer_thread.joinable()) timer_thread.join();

    cout << "\n=== Simulation Summary ===\n";
    cout << "Submitted: " << thread_pool.get_submitted_task_count() << endl;
    cout << "Completed: " << thread_pool.get_completed_task_count() << endl;
    cout << "Rejected : " << thread_pool.get_rejected_task_count() << endl;
    cout << "Avg Wait : " << thread_pool.get_avg_wait_time() << "s\n";
    cout << "Max Full : " << thread_pool.get_max_queue_full_time() << "ms\n";
    cout << "Min Full : " << thread_pool.get_min_queue_full_time() << "ms\n";

    return 0;
}
