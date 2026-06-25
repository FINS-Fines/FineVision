/*******************************************************************************
 * Copyright (c) 2025.
 * IWIN-FINS Lab, Shanghai Jiao Tong University, Shanghai, China.
 * All rights reserved.
 ******************************************************************************/

#include <fins/utils/performance_recorder.hpp>
#include <fins/third_party/json.hpp>
#include <fins/utils/logger.hpp>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

namespace fins {

PerformanceMonitor& PerformanceMonitor::get_instance() {
    static PerformanceMonitor instance;
    return instance;
}

PerformanceMonitor::PerformanceMonitor() : running_(false) {}

PerformanceMonitor::~PerformanceMonitor() { stop(); }

void PerformanceMonitor::push_record(MsgPerfRecord&& record) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(record));
        if (queue_.size() > 5000) {
            queue_.pop_front();
        }
    }
    cv_.notify_one();
}

void PerformanceMonitor::start(const std::string& filename) {
    if (running_) return;
    
    std::string home_dir = std::string(getenv("HOME")) + "/.fins/performance";
    struct stat st = {};
    if (stat(home_dir.c_str(), &st) == -1) {
        mkdir(home_dir.c_str(), 0755);
    }
    
    if (filename.empty()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        char buffer[64];
        std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm);
        filename_ = home_dir + "/runtime_" + buffer + ".jsonl";
    } else {
        filename_ = home_dir + "/" + filename;
    }
    
    running_ = true;
    worker_thread_ = std::thread(&PerformanceMonitor::worker_loop, this);
}

void PerformanceMonitor::stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
        FINS_LOG_INFO("[Perf] Performance data saved to: {}", filename_);
    }
}

void PerformanceMonitor::worker_loop() {
    pthread_setname_np(pthread_self(), "fins_perf_monitor");
    std::ofstream ofs(filename_, std::ios::app);
    while (running_ || !queue_.empty()) {
        std::deque<MsgPerfRecord> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this] { 
                return !queue_.empty() || !running_; 
            });
            batch.swap(queue_);
        }

        for (const auto& r : batch) {
            nlohmann::json j;
            j["id"] = r.node_id;
            j["p"] = r.port;
            j["port_desc"] = r.port_desc;
            j["acq"] = r.acq_time_ns;
            j["recv"] = r.recv_time_ns;
            j["comp"] = r.comp_time_ns;
            j["lat_ms"] = (r.comp_time_ns - r.recv_time_ns) / 1000000.0;
            j["sys_lat_ms"] = (r.recv_time_ns - r.acq_time_ns) / 1000000.0;
            j["cpu_ms"] = r.cpu_duration_ns / 1000000.0;
            j["sched_wait_ms"] = (r.comp_time_ns - r.recv_time_ns - r.cpu_duration_ns) / 1000000.0;
            j["tid"] = r.thread_id;
            ofs << j.dump() << "\n";
        }
        ofs.flush();
    }
}

ScopedSegmentTimer::ScopedSegmentTimer(std::string node_id, std::string segment_name, AcqTime acq_time)
    : node_id_(std::move(node_id)), segment_name_(std::move(segment_name)), 
      acq_ts_(acq_time), active_(true) {
    start_ts_ = fins::now();
    start_cpu_ns_ = get_thread_cpu_time_ns();
}

ScopedSegmentTimer::ScopedSegmentTimer(ScopedSegmentTimer&& other) noexcept 
    : node_id_(std::move(other.node_id_)), segment_name_(std::move(other.segment_name_)),
      acq_ts_(other.acq_ts_), start_ts_(other.start_ts_), start_cpu_ns_(other.start_cpu_ns_), active_(other.active_) {
    other.active_ = false;
}

ScopedSegmentTimer::ScopedSegmentTimer(std::string node_id, std::string segment_name, double acq_time_sec)
    : ScopedSegmentTimer(node_id, segment_name, fins::from_seconds(acq_time_sec)) {}

ScopedSegmentTimer& ScopedSegmentTimer::operator=(ScopedSegmentTimer&& other) noexcept {
    if (this != &other) {
        node_id_ = std::move(other.node_id_);
        segment_name_ = std::move(other.segment_name_);
        acq_ts_ = other.acq_ts_;
        start_ts_ = other.start_ts_;
        start_cpu_ns_ = other.start_cpu_ns_;
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

ScopedSegmentTimer::~ScopedSegmentTimer() {
    if (active_) {
        auto end_ts_ = fins::now();
        auto end_cpu_ns_ = get_thread_cpu_time_ns();
        PerformanceMonitor::get_instance().push_record({
            node_id_, -1, segment_name_,
            to_nanoseconds(acq_ts_),
            to_nanoseconds(start_ts_),
            to_nanoseconds(end_ts_),
            (end_cpu_ns_ - start_cpu_ns_),
            (int64_t)pthread_self()
        });
    }
}

} // namespace fins