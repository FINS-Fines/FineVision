/*******************************************************************************
 * Copyright (c) 2025.
 * IWIN-FINS Lab, Shanghai Jiao Tong University, Shanghai, China.
 * All rights reserved.
 *******************************************************************************/

// main.cpp

#include <fins/server/server.hpp>
#include <fins/server/ros2_manager.hpp>
#include <fins/node_log.hpp>
#include <fins/thread_manager.hpp>
#include <fins/utils/performance_recorder.hpp>
#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>

namespace {
  std::atomic<bool> g_running(true);
  void signal_handler(int) {
    g_running = false;
  }
}

void print_usage(const char *prog_name) {
  std::cout << "Usage: " << prog_name << " [options]\n"
            << "Options:\n"
            << "  --threads-urgent <n>    Set urgent priority thread pool size (default: 4)\n"
            << "  --threads-high <n>      Set high priority thread pool size (default: 4)\n"
            << "  --threads-medium <n>    Set medium priority thread pool size (default: 4)\n"
            << "  --threads-low <n>       Set low priority thread pool size (default: 4)\n"
            << "  --log-level <level>     Set node log level (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=OFF) (default: 1)\n"
            << "  --perf                  Enable performance monitor (default: off)\n"
            << "  --workspace <path>      Add workspace install path for plugin loading (can be repeated)\n"
            << "  --webui <url>           Connect to WebUI URL (e.g. http://localhost:8080)\n"
            << "  --name <agent_name>     Set agent name (default: agent)\n"
            << "  --ip <agent_ip>         Set agent IP binding (default: 0.0.0.0)\n"
            << "  --port <agent_port>     Set agent listening port (default: 9090)\n"
            << "  --terminal-log <on/off> Enable/disable terminal log printing (default: on)\n"
            << "  -h, --help              Show this help message\n";
}

int main(int argc, char **argv) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  int urgent_threads = 2;
  int high_threads = 2;
  int medium_threads = 0;
  int low_threads = 0;
  int log_level = 1; // INFO
  bool terminal_log = true;
  bool enable_perf = false;
  std::vector<std::string> workspaces;
  std::string webui_url = "http://localhost:8080";
  std::string agent_name = "agent";
  std::string agent_ip = "0.0.0.0";
  int agent_port = 9090;

  struct option long_options[] = {{"log-level", required_argument, 0, 'L'},
                                  {"perf", no_argument, 0, 'f'},
                                  {"workspace", required_argument, 0, 'W'},
                                  {"webui", required_argument, 0, 'w'},
                                  {"name", required_argument, 0, 'n'},
                                  {"ip", required_argument, 0, 'I'},
                                  {"port", required_argument, 0, 'P'},
                                  {"terminal-log", required_argument, 0, 'T'},
                                  {"help", no_argument, 0, 'h'},
                                  {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "u:H:m:l:L:f:W:w:n:I:P:T:h", long_options, &option_index)) != -1) {
    switch (opt) {
      case 'L':
        log_level = std::stoi(optarg);
        break;
      case 'f':
        enable_perf = true;
        break;
      case 'W':
        workspaces.push_back(optarg);
        break;
      case 'T': {
        std::string val = optarg;
        if (val == "off" || val == "0" || val == "false") {
          terminal_log = false;
        } else {
          terminal_log = true;
        }
        break;
      }
      case 'w':
        webui_url = optarg;
        break;
      case 'n':
        agent_name = optarg;
        break;
      case 'I':
        agent_ip = optarg;
        break;
      case 'P':
        agent_port = std::stoi(optarg);
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  // Set Log Level
  if (log_level < 0)
    log_level = 0;
  if (log_level > 4)
    log_level = 4;
  fins::set_node_log_level(static_cast<fins::NodeLogLevel>(log_level));
  fins::Logger::get().set_node_terminal_enabled(terminal_log);

  FINS_THREAD_MANAGER.start();

  if (enable_perf) {
    FINS_PERF_MONITOR.start();
  }

  fins::NodeLib lib;

  fins::AgentServer server(lib);

  server.connect(webui_url);

  // Load plugins first — plugins (e.g. rosbridge) may initialize rclcpp themselves.
  // Ros2Manager initializes lazily after plugins, reusing any existing rclcpp context.
  lib.load_directory("~/.fins/install/");
  for (const auto &ws : workspaces) {
    std::string install_path = ws + "/install/";
    FINS_LOG_INFO("[Agent] Loading plugins from workspace: {}", install_path);
    lib.load_directory(install_path);
  }

  // Initialize ROS2 support after plugins (no-op if ROS2 not available).
  // If a plugin already called rclcpp::init(), this reuses that context.
  fins::Ros2Manager::get_instance().initialize(argc, argv, agent_name);

  server.start(agent_name, agent_ip, agent_port);

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  FINS_LOG_INFO("[Agent] Shutting down agent...");
  server.stop();
  FINS_PERF_MONITOR.stop();
  FINS_STUDIO.clear();
  FINS_THREAD_MANAGER.shutdown();
  fins::Ros2Manager::get_instance().shutdown();

  return 0;
}
