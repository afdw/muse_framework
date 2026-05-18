#pragma once

#include "audioworkgroup.h"
#include "concurrency/threadutils.h"
#include "thirdparty/moodycamel/blockingconcurrentqueue.h"
#include "thirdparty/sg14/inplace_function.h"

#include "thirdparty/moodycamel/lightweightsemaphore.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace muse::audio {
class RealtimeThreadPool
{
public:
    static constexpr int maxTaskCount = 10000;
    RealtimeThreadPool(std::string name, int num_of_workers = std::thread::hardware_concurrency())
    {
        for (size_t i = 0; i < static_cast<size_t>(num_of_workers); ++i) {
            auto worker = std::make_unique<Worker>();
            Worker* workerPtr = worker.get();
            const size_t workerIndex = i;
            worker->m_thread = std::make_unique<std::thread>([this, workerPtr, workerIndex, name] {
                {
#if defined __linux__
                    std::string thread_name = name + " worker " + std::to_string(workerIndex);
                    pthread_setname_np(pthread_self(), thread_name.c_str());
#elif defined __APPLE__
                    std::string thread_name = name + " worker " + std::to_string(workerIndex);
                    pthread_setname_np(thread_name.c_str());
#endif
                }
                AudioWorkgroupToken workgroupToken;
                for (;;) {
                    stdext::inplace_function<void()> task;

                    m_queue.wait_dequeue(task);
                    {
                        std::lock_guard lock(workerPtr->m_workgroupMutex);
                        workerPtr->m_workgroup.join(workgroupToken);
                    }

                    if (this->m_should_stop) {
                        return;
                    }

                    task();
                    m_inflightSemaphore.signal();
                }
            });
            m_workers.push_back(std::move(worker));
            muse::setThreadPriority(*m_workers.back()->m_thread, ThreadPriority::High);
        }
    }

    void setAudioWorkgroup(muse::audio::AudioWorkGroup audioworkgroup)
    {
        for (auto& worker : m_workers) {
            // this lock is just for safety. Normally threads should not be busy during this function.
            std::lock_guard<std::mutex> workerLock(worker->m_workgroupMutex);
            worker->m_workgroup = audioworkgroup;
        }
    }

    void enqueue(const stdext::inplace_function<void()>& func)
    {
        m_inflightSemaphore.wait();
        m_queue.enqueue(func);
    }

    void participateAndWait()
    {
        stdext::inplace_function<void()> task;
        while (m_queue.try_dequeue(task)) {
            task();
            m_inflightSemaphore.signal();
        }
        waitUntilFinished();
        m_inflightSemaphore.signal(maxTaskCount);
    }

    void waitUntilFinished()
    {
        auto actuallyAwaited = m_inflightSemaphore.waitMany(maxTaskCount);
        for (size_t i = 0;
             i < static_cast<size_t>(maxTaskCount - actuallyAwaited); ++i) {
            m_inflightSemaphore.wait();
        }
    }

    std::set<std::thread::id> threadIdSet() const
    {
        std::set<std::thread::id> result;

        for (const auto& worker : m_workers) {
            result.insert(worker->m_thread->get_id());
        }

        return result;
    }

    ~RealtimeThreadPool()
    {
        m_should_stop = true;
        for (size_t i = 0; i < m_workers.size(); ++i) {
            m_queue.enqueue([] {});
        }

        for (auto& worker : m_workers) {
            if (worker->m_thread && worker->m_thread->joinable()) {
                worker->m_thread->join();
            }
        }
    }

private:
    struct Worker {
        std::unique_ptr<std::thread> m_thread;
        AudioWorkGroup m_workgroup;
        std::mutex m_workgroupMutex;
    };
    moodycamel::BlockingConcurrentQueue<stdext::inplace_function<void()> > m_queue;
    std::vector<std::unique_ptr<Worker> > m_workers;
    moodycamel::LightweightSemaphore m_inflightSemaphore { maxTaskCount };
    std::atomic<bool> m_should_stop{ false };
}; // namespace muse::audio
} // namespace muse::audio
