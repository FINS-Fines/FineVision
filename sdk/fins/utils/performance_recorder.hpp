#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <fins/utils/time.hpp>

namespace fins {

struct MsgPerfRecord {
    std::string node_id;
    int port;
    std::string port_desc;
    int64_t acq_time_ns;
    int64_t recv_time_ns;
    int64_t comp_time_ns;
    int64_t cpu_duration_ns;
    int64_t thread_id;
};

class PerformanceMonitor {
public:
    static PerformanceMonitor& get_instance();

    void push_record(MsgPerfRecord&& record);
    void start(const std::string& filename = "");
    void stop();

private:
    PerformanceMonitor();
    ~PerformanceMonitor();

    void worker_loop();

    std::deque<MsgPerfRecord> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::string filename_;
};

class ScopedSegmentTimer {
public:
    ScopedSegmentTimer(std::string node_id, std::string segment_name, AcqTime acq_time);
    ScopedSegmentTimer(ScopedSegmentTimer&& other) noexcept;
    ScopedSegmentTimer(std::string node_id, std::string segment_name, double acq_time_sec);
    ScopedSegmentTimer& operator=(ScopedSegmentTimer&& other) noexcept;

    ScopedSegmentTimer(const ScopedSegmentTimer&) = delete;
    ScopedSegmentTimer& operator=(const ScopedSegmentTimer&) = delete;

    ~ScopedSegmentTimer();

private:
    std::string node_id_;
    std::string segment_name_;
    AcqTime acq_ts_;
    AcqTime start_ts_;
    int64_t start_cpu_ns_;
    bool active_;
};

#define FINS_PERF_MONITOR fins::PerformanceMonitor::get_instance()
}