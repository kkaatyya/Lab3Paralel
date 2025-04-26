#include <iostream>
#include <functional>
#include <queue>
#include <shared_mutex>

// Типи для зручної роботи з блокуванням читання/запису
using rw_lock_t = std::shared_mutex;
using shared_read_lock = std::shared_lock<rw_lock_t>;
using exclusive_write_lock = std::unique_lock<rw_lock_t>;

// Структура задачі з пріоритетом
struct TaskWithPriority
{
    int priority;
    std::function<void()> task;

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
    using PriorityQueueImpl = std::priority_queue<TaskType>;

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

int main()
{
    TaskQueue<TaskWithPriority> queue(5);

    for (int i = 5; i >= 1; --i)
    {
        TaskWithPriority task{
            i,
            [i]() { std::cout << "Running task with priority " << i << std::endl; }
        };

        if (queue.push_if_possible(task))
        {
            std::cout << "Task with priority " << i << " added.\n";
        }
        else
        {
            std::cout << "Task with priority " << i << " rejected (queue full).\n";
        }
    }

    std::cout << "\nExecuting tasks in priority order:\n";

    while (!queue.empty())
    {
        TaskWithPriority task;
        if (queue.pop(task))
        {
            if (task.task)
            {
                task.task();
            }
        }
    }

    return 0;
}
