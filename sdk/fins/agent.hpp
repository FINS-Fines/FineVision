/*******************************************************************************
 * Copyright (c) 2025.
 * IWIN-FINS Lab, Shanghai Jiao Tong University, Shanghai, China.
 * All rights reserved.
 ******************************************************************************/

// agent.hpp — C++ bringup API: Group builder + Agent launcher + WebUI
//
//   auto g = fins::group()
//       .node({.name="StringGenerator", .outputs={{"out","/s"}}, .parameters={{"msg","hi"}}})
//       .node({.name="StringPrinter",  .inputs={{"in","/s"}}, .parameters={{"pfx","[X]"}}});
//
//   auto a = fins::agent("0.0.0.0", 1896);
//   a.load_plugins();
//   a.launch(g);
//   a.run();

#pragma once

#include <fins/node.hpp>
#include <fins/nodelib.hpp>
#include <fins/pipe.hpp>
#include <fins/server/ros2_manager.hpp>
#include <fins/server/server.hpp>
#include <fins/studio.hpp>
#include <fins/thread_manager.hpp>
#include <fins/utils/logger.hpp>
#include <fins/utils/performance_recorder.hpp>
#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fins {

namespace fs = std::filesystem;

// ── NodeArgs ───────────────────────────────────────────────────────
struct NodeArgs {
    std::string name;
    std::map<std::string, std::string> inputs;
    std::map<std::string, std::string> outputs;
    std::map<std::string, std::string> parameters;
};

// ── helpers ─────────────────────────────────────────────────────────
inline std::string _random_id(const std::string& prefix) {
    static std::mt19937_64 rng(std::random_device{}());
    static const char hex[] = "0123456789abcdef";
    std::string s(6, '\0');
    for (auto& c : s) c = hex[rng() & 15];
    return prefix + "_" + s;
}

// ── Group (node collection builder) ─────────────────────────────────
class Group {
public:
    Group() = default;

    Group& node(NodeArgs args) {
        std::string id = _random_id(args.name);
        specs_[id] = std::move(args);
        order_.push_back(id);
        return *this;
    }

private:
    friend class Agent;
    std::map<std::string, NodeArgs> specs_;
    std::vector<std::string> order_;
};

inline Group group() { return Group{}; }

// ── Agent (lifecycle manager + WebUI) ───────────────────────────────
class Agent {
public:
    Agent() = default;

    /// @param bind_ip    Agent TCP server bind address (e.g. "0.0.0.0")
    /// @param bind_port  Agent TCP server port
    /// @param webui_url  WebUI orchestrator URL (e.g. "http://localhost:8080"), empty = no WebUI
    Agent(const std::string& bind_ip, int bind_port,
          const std::string& webui_url = "")
        : bind_ip_(bind_ip), bind_port_(bind_port), webui_url_(webui_url) {}

    /// Set the agent name (reported to WebUI, appears in /rosout).
    Agent& set_name(const std::string& name) { name_ = name; return *this; }

    // ── Plugin loading ───────────────────────────────────────────────

    Agent& load_plugins() {
        // ~/.fins/install/
        std::string home = expand_user("~/.fins/install/");
        FINS_LOG_INFO("[Agent] Scanning: {}", home);
        if (fs::exists(home) && fs::is_directory(home))
            lib_.load_directory(home);

        // Colcon workspace install/ — try relative to cwd first,
        // then relative to the executable (for ros2 launch).
        std::string install_dir = find_install_dir();
        if (!install_dir.empty()) {
            FINS_LOG_INFO("[Agent] Workspace install: {}", install_dir);
            for (auto& pkg : fs::directory_iterator(install_dir)) {
                auto lib_dir = pkg.path() / "lib";
                if (fs::exists(lib_dir) && fs::is_directory(lib_dir)) {
                    FINS_LOG_INFO("[Agent] Scanning: {}", lib_dir.string());
                    lib_.load_directory(lib_dir.string());
                }
            }
        }

        FINS_LOG_INFO("[Agent] {} node(s) registered", NodeFactory::get_instance().count());
        return *this;
    }

    Agent& load_plugins_from(const std::vector<std::string>& dirs) {
        for (auto& d : dirs) {
            std::string path = expand_user(d);
            FINS_LOG_INFO("[Agent] Scanning: {}", path);
            if (fs::exists(path) && fs::is_directory(path))
                lib_.load_directory(path);
        }
        return *this;
    }

    // ── Launch ───────────────────────────────────────────────────────

    Agent& launch(const Group& g) {
        auto& studio = Studio::GetInstance();
        json dataflow;
        dataflow["nodes"] = json::array();

        for (auto& id : g.order_) {
            auto& spec = g.specs_.at(id);
            INode* node = find_and_create(spec.name);
            if (!node) {
                throw std::runtime_error("[Agent] Node type '" + spec.name
                    + "' not found. Did you call load_plugins()?");
            }
            auto step = studio.add_step(std::shared_ptr<INode>(node), id);

            for (auto& [port, pipe_id] : spec.outputs)
                step->add_output_pipe(find_port(step->get_node_meta().outputs, port), pipe_id);
            for (auto& [port, pipe_id] : spec.inputs)
                step->add_input_pipe(find_port(step->get_node_meta().inputs, port), pipe_id);
            for (auto& [key, val] : spec.parameters)
                studio.set_step_parameter(id, key, val);

            launched_.push_back(id);

            // Build JSON entry for /get_dataflow
            json j;
            j["id"] = id;
            j["name"] = spec.name;
            if (!spec.inputs.empty()) {
                j["inputs"] = json::object();
                for (auto& [port, pipe_id] : spec.inputs)
                    j["inputs"][port] = {{"connect", pipe_id}};
            }
            if (!spec.outputs.empty()) {
                j["outputs"] = json::object();
                for (auto& [port, pipe_id] : spec.outputs)
                    j["outputs"][port] = pipe_id;
            }
            if (!spec.parameters.empty()) {
                j["parameters"] = json::array();
                for (auto& [key, val] : spec.parameters)
                    j["parameters"].push_back({{"name", key}, {"value", val}});
            }
            dataflow["nodes"].push_back(j);
        }

        // Store for /get_dataflow (set_dataflow_json only stores,
        // it does NOT clear/reload Studio like load_json does)
        lib_.set_dataflow_json(dataflow.dump());

        studio.set_topology_order(launched_);
        return *this;
    }

    // ── Optional runtime services ────────────────────────────────────

    Agent& start_threadpool(int n_threads = 4) {
        FINS_LOG_INFO("[Agent] Starting thread pool ({} threads)", n_threads);
        FINS_THREAD_MANAGER.start();
        threadpool_on_ = true;
        return *this;
    }

    Agent& start_timeline_monitor() {
        FINS_LOG_INFO("[Agent] Starting timeline monitor");
        FINS_TIMELINE_MONITOR.start();
        timeline_on_ = true;
        return *this;
    }

    Agent& start_ros2manager() {
        char* dummy_argv[] = {const_cast<char*>("agent"), nullptr};
        int dummy_argc = 1;
        Ros2Manager::get_instance().initialize(dummy_argc, dummy_argv, "agent");
        ros2_on_ = true;
        FINS_LOG_INFO("[Agent] ROS 2 manager started");
        return *this;
    }

    // ── Run ──────────────────────────────────────────────────────────

    void run() {
        // Agent TCP server (WebUI registration is optional)
        if (!bind_ip_.empty()) {
            server_ = std::make_unique<AgentServer>(lib_);
            if (!webui_url_.empty()) {
                server_->connect(webui_url_);
                FINS_LOG_INFO("[Agent] WebUI: {}", webui_url_);
            }
            server_->start(name_.empty() ? "agent" : name_, bind_ip_, bind_port_);
            FINS_LOG_INFO("[Agent] TCP server on {}:{}", bind_ip_, bind_port_);
        }

        // Initialize nodes (matches JSON load path)
        auto& studio = Studio::GetInstance();
        for (auto& id : launched_) {
            auto step = studio.get_step(id);
            if (step && step->get_node())
                step->get_node()->initialize();
        }

        studio.run();
    }

    // ── Stop ─────────────────────────────────────────────────────────

    void stop() {
        if (server_) { server_->stop(); server_.reset(); }
        if (timeline_on_) FINS_TIMELINE_MONITOR.stop();
        Studio::GetInstance().pause();
        Studio::GetInstance().clear();
        if (threadpool_on_) FINS_THREAD_MANAGER.shutdown();
        if (ros2_on_) Ros2Manager::get_instance().shutdown();
    }

    void reset() { Studio::GetInstance().reset(); }

private:
    NodeLib lib_;
    std::string name_;
    std::string bind_ip_;
    int bind_port_ = 1896;
    std::string webui_url_;
    std::unique_ptr<AgentServer> server_;
    std::vector<std::string> launched_;
    bool threadpool_on_ = false;
    bool timeline_on_ = false;
    bool ros2_on_ = false;

    static int find_port(const std::vector<PortInfo>& ports, const std::string& name) {
        for (size_t i = 0; i < ports.size(); ++i)
            if (ports[i].name == name) return static_cast<int>(i);
        throw std::runtime_error("[Agent] Port '" + name + "' not found");
    }

    static INode* find_and_create(const std::string& type) {
        auto& factory = NodeFactory::get_instance();
        for (size_t i = 0; i < factory.count(); ++i) {
            std::string key = factory.get_name(i);
            auto slash = key.rfind('/');
            std::string tail = (slash != std::string::npos) ? key.substr(slash + 1) : key;
            auto at = tail.find('@');
            if ((at != std::string::npos ? tail.substr(0, at) : tail) == type) {
                FINS_LOG_INFO("[Agent] Creating: {}", key);
                return factory.create(key);
            }
        }
        FINS_LOG_ERROR("[Agent] '{}' not found. Registered ({}):", type, factory.count());
        for (size_t i = 0; i < factory.count(); ++i)
            FINS_LOG_ERROR("  - {}", factory.get_name(i));
        return nullptr;
    }

    // Walk up from cwd or executable path to find colcon install/
    static std::string find_install_dir() {
        // 1. cwd
        if (fs::exists("install") && fs::is_directory("install"))
            return "install";

        // 2. Walk up from executable path (for ros2 launch)
        std::string exe = expand_user("/proc/self/exe");
        if (fs::exists(exe) && fs::is_symlink(exe)) {
            auto dir = fs::read_symlink(exe).parent_path();
            while (!dir.empty() && dir != dir.root_directory()) {
                auto candidate = dir / "install";
                if (fs::exists(candidate) && fs::is_directory(candidate))
                    return candidate.string();
                dir = dir.parent_path();
            }
        }
        return "";
    }

    static std::string expand_user(const std::string& path) {
        if (path.empty() || path[0] != '~') return path;
        const char* home = getenv("HOME");
        return home ? std::string(home) + path.substr(1) : path;
    }
};

inline Agent agent() { return Agent{}; }
inline Agent agent(const std::string& bind_ip, int bind_port,
                   const std::string& webui_url = "") {
    return Agent{bind_ip, bind_port, webui_url};
}

} // namespace fins
