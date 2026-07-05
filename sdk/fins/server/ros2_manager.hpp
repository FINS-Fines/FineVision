/*******************************************************************************
 * Copyright (c) 2025.
 * IWIN-FINS Lab, Shanghai Jiao Tong University, Shanghai, China.
 * All rights reserved.
 ******************************************************************************/

// ros2_manager.hpp

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fins/node_log.hpp>
#include <fins/server/parameter_server.hpp>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

#if __has_include(<rclcpp/rclcpp.hpp>)
  #include <rclcpp/rclcpp.hpp>
  #include <rcl_interfaces/msg/log.hpp>
  #define FINS_HAS_ROS2 1
#endif

namespace fins {

/**
 * @brief ROS2 管理器（单例） / ROS2 Manager (singleton)
 * @details 当 ROS2 环境可用时，管理 rosout 日志发布和参数服务器桥接。
 *          所有 rclcpp::Node 操作（declare_parameter、spin、publish）仅限于
 *          专用 spin 线程执行，避免与 HTTP 服务器线程产生竞态条件。
 *
 *          When ROS2 is available, manages rosout log publishing and
 *          parameter server bridging. All rclcpp::Node operations
 *          (declare_parameter, spin, publish) are confined to the
 *          dedicated spin thread to avoid races with HTTP server threads.
 */
class Ros2Manager {
public:
  /// @brief 获取单例 / Get singleton instance
  static Ros2Manager &get_instance() {
    static Ros2Manager instance;
    return instance;
  }

  Ros2Manager(const Ros2Manager &) = delete;
  Ros2Manager &operator=(const Ros2Manager &) = delete;

  /**
   * @brief 初始化 ROS2 支持 / Initialize ROS2 support
   */
  void initialize(int argc, char **argv, const std::string &node_name) {
#ifdef FINS_HAS_ROS2
    if (initialized_)
      return;

    if (!rclcpp::ok()) {
      rclcpp::init(argc, argv);
    }

    node_ = std::make_shared<rclcpp::Node>(node_name);

    // Setup rosout publisher
    // Match standard ROS2 /rosout QoS (rcl_logging_rosout: KeepLast(100), reliable, volatile)
    auto rosout_qos = rclcpp::QoS(rclcpp::KeepLast(100));
    rosout_pub_ = node_->create_publisher<rcl_interfaces::msg::Log>("/rosout", rosout_qos);

    // Register log sink (called from any NodeLogger thread — publishing is thread-safe)
    register_log_sink([this](const LogEntry &entry) { on_log_entry(entry); });

    // Register parameter change callback from ROS2 (called from spin thread)
    param_callback_handle_ = node_->add_on_set_parameters_callback(
        [this](const std::vector<rclcpp::Parameter> &params) {
          return on_set_parameters(params);
        });

    // Register for ParameterServer changes (thread-safe: just sets a flag)
    ParameterServer::get_instance().set_on_params_changed([this]() { sync_parameters(); });

    // Initial parameter sync (done on spin thread via flag)
    params_dirty_ = true;

    // Start spin thread — all Node operations happen here
    running_ = true;
    spin_thread_ = std::make_unique<std::thread>([this]() {
      while (running_ && rclcpp::ok()) {
        // Process parameter sync requests (thread-safe: only this thread touches node_)
        if (params_dirty_.exchange(false)) {
          do_sync_parameters();
        }

        rclcpp::spin_some(node_);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    initialized_ = true;
    FINS_LOG_INFO("[Ros2Manager] Initialized with node '{}'", node_name);
#else
    (void)argc;
    (void)argv;
    (void)node_name;
#endif
  }

  /**
   * @brief 关闭 ROS2 支持 / Shutdown ROS2 support
   */
  void shutdown() {
#ifdef FINS_HAS_ROS2
    if (!initialized_)
      return;

    running_ = false;
    if (spin_thread_ && spin_thread_->joinable()) {
      spin_thread_->join();
    }
    spin_thread_.reset();

    rosout_pub_.reset();
    param_callback_handle_.reset();
    node_.reset();

    if (rclcpp::ok()) {
      rclcpp::shutdown();
    }

    initialized_ = false;
    FINS_LOG_INFO("[Ros2Manager] Shutdown complete");
#endif
  }

  /**
   * @brief 检查 ROS2 是否可用 / Check if ROS2 is available
   */
  bool is_available() const {
#ifdef FINS_HAS_ROS2
    return true;
#else
    return false;
#endif
  }

  /**
   * @brief 请求同步参数到 ROS2（线程安全） / Request parameter sync to ROS2 (thread-safe)
   * @details 设置脏标志，由 spin 线程在下一次循环中执行实际同步。
   *          可从任意线程安全调用。
   *
   *          Sets a dirty flag; the spin thread performs the actual sync
   *          on its next cycle. Safe to call from any thread.
   */
  void sync_parameters() {
#ifdef FINS_HAS_ROS2
    params_dirty_ = true;
#endif
  }

private:
  Ros2Manager() = default;
  ~Ros2Manager() { shutdown(); }

#ifdef FINS_HAS_ROS2
  void on_log_entry(const LogEntry &entry) {
    if (!rosout_pub_ || !node_)
      return;

    rcl_interfaces::msg::Log msg;
    msg.stamp = node_->now();

    if (entry.level == "DEBUG")
      msg.level = rcl_interfaces::msg::Log::DEBUG;
    else if (entry.level == "INFO")
      msg.level = rcl_interfaces::msg::Log::INFO;
    else if (entry.level == "WARN")
      msg.level = rcl_interfaces::msg::Log::WARN;
    else if (entry.level == "ERROR")
      msg.level = rcl_interfaces::msg::Log::ERROR;
    else
      msg.level = rcl_interfaces::msg::Log::INFO;

    msg.name = node_->get_name();
    msg.msg = entry.message;
    msg.file = entry.file;
    msg.function = "";
    msg.line = entry.line;

    rosout_pub_->publish(msg);
  }

  rcl_interfaces::msg::SetParametersResult
  on_set_parameters(const std::vector<rclcpp::Parameter> &params) {
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = true;

    for (const auto &param : params) {
      try {
        std::string key = param.get_name();
        std::string value = param.value_to_string();
        ParameterServer::get_instance().set_value(key, value);
        FINS_LOG_INFO("[Ros2Manager] Parameter '{}' updated to '{}' via ROS2", key, value);
      } catch (const std::exception &e) {
        result.successful = false;
        result.reason = std::string("Failed to set parameter: ") + e.what();
        FINS_LOG_ERROR("[Ros2Manager] Parameter update failed: {}", e.what());
      }
    }

    return result;
  }

  /**
   * @brief 执行参数同步（仅从 spin 线程调用） / Perform parameter sync (called from spin thread only)
   */
  void do_sync_parameters() {
    if (!node_)
      return;

    auto keys = ParameterServer::get_instance().get_all_keys();
    for (const auto &key : keys) {
      if (declared_params_.find(key) != declared_params_.end())
        continue;

      std::string raw_val = ParameterServer::get_instance().get_raw_value(key);
      if (raw_val.empty())
        continue;

      // Try to declare as int first, then double, then bool, then fallback to string
      try {
        size_t pos = 0;
        double dval = std::stod(raw_val, &pos);
        if (pos == raw_val.size()) {
          if (raw_val.find('.') == std::string::npos) {
            try {
              int64_t ival = std::stoll(raw_val);
              node_->declare_parameter<int64_t>(key, ival);
            } catch (...) {
              node_->declare_parameter<double>(key, dval);
            }
          } else {
            node_->declare_parameter<double>(key, dval);
          }
          declared_params_.insert(key);
          continue;
        }
      } catch (...) {
      }

      // Check for bool
      std::string lower = raw_val;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      if (lower == "true" || lower == "false") {
        node_->declare_parameter<bool>(key, lower == "true");
        declared_params_.insert(key);
        continue;
      }

      // Fallback: declare as string
      node_->declare_parameter<std::string>(key, raw_val);
      declared_params_.insert(key);
    }
  }

  std::shared_ptr<rclcpp::Node> node_;
  rclcpp::Publisher<rcl_interfaces::msg::Log>::SharedPtr rosout_pub_;
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr param_callback_handle_;
  std::unique_ptr<std::thread> spin_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> params_dirty_{false};
  bool initialized_{false};
  std::unordered_set<std::string> declared_params_;
#endif
};

} // namespace fins
