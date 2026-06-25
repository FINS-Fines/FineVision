/*******************************************************************************
 * Copyright (c) 2025.
 * IWIN-FINS Lab, Shanghai Jiao Tong University, Shanghai, China.
 * All rights reserved.
 ******************************************************************************/

#include <fins/node.hpp>
#include <fins/node_log.hpp>
#include <fins/third_party/json.hpp>
#include <fins/utils/performance_recorder.hpp>
#include <fins/server/parameter_server.hpp>
#include <fins/service/service_manager.hpp>
#include <fins/action/action_manager.hpp>
#include <cassert>

namespace fins {

  // =========================================================================
  // NodeMeta Implementation
  // =========================================================================
  std::string NodeMeta::to_json_string() const {
    nlohmann::json j;
    j["name"] = name;
    j["description"] = description;
    j["category"] = category;
    j["source"] = source;
    j["package_name"] = package_name;
    j["version"] = version;

    auto map_ports = [](const std::vector<PortInfo> &ports) {
      nlohmann::json arr = nlohmann::json::array();
      for (size_t i = 0; i < ports.size(); ++i) {
        arr.push_back({
            {"id", i},
            {"name", ports[i].name},
            {"type", ports[i].type},
        });
      }
      return arr;
    };

    auto map_parameters = [](const std::vector<ParameterInfo> &params) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &e: params) {
        arr.push_back({
            {"name", e.name},
            {"type", e.type},
            {"default_value", e.default_value},
        });
      }
      return arr;
    };

    auto map_services = [](const std::vector<ServiceInfo> &svcs) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &s: svcs) {
        arr.push_back({{"name", s.name}, {"request_type", s.request_type}, {"response_type", s.response_type}});
      }
      return arr;
    };

    auto map_actions = [](const std::vector<ActionInfo> &acts) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &a: acts) {
        arr.push_back({{"name", a.name}, {"goal_type", a.goal_type}, {"feedback_type", a.feedback_type}});
      }
      return arr;
    };

    j["inputs"] = map_ports(inputs);
    j["outputs"] = map_ports(outputs);
    j["parameters"] = map_parameters(parameters);
    j["clients"] = map_services(clients);
    j["servers"] = map_services(servers);
    j["commanders"] = map_actions(commanders);
    j["actors"] = map_actions(actors);
    
    std::string priority_str;
    switch (schedule.priority) {
      case SchedulePriority::Urgent: priority_str = "Urgent"; break;
      case SchedulePriority::High: priority_str = "High"; break;
      case SchedulePriority::Medium: priority_str = "Medium"; break;
      case SchedulePriority::Low: priority_str = "Low"; break;
    }
    std::string queue_str = (schedule.queue == ScheduleQueue::FCFS) ? "FCFS" : "LGFS";
    j["schedule"] = {
      {"priority", priority_str},
      {"queue", queue_str}
    };
    
    return j.dump();
  }

  // =========================================================================
  // INode Implementation
  // =========================================================================
  void INode::define() {
    FINS_LOG_WARN("[Node {} Warning] define() not implemented.", get_meta().name);
  }
  void INode::initialize() {
    FINS_LOG_WARN("[Node {} Warning] initialize() not implemented.", get_meta().name);
  }
  void INode::run() {
    FINS_LOG_WARN("[Node {} Warning] run() not implemented.", get_meta().name);
  }
  void INode::pause() {
    FINS_LOG_WARN("[Node {} Warning] pause() not implemented.", get_meta().name);
  }
  void INode::reset() {
    FINS_LOG_WARN("[Node {} Warning] reset() not implemented.", get_meta().name);
  }

  // =========================================================================
  // Node Implementation
  // =========================================================================
  Node::Node() : logger(std::make_shared<NodeLogger>()) {
#ifdef PKG_SOURCE
    std::string src = PKG_SOURCE;
    if (src.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@.") != std::string::npos) {
      throw std::runtime_error("[Node " + meta_.name + "]: Invalid source format in PKG_SOURCE: " + src);
    }
    meta_.source = src;
#endif
#ifdef PKG_NAME
    meta_.package_name = PKG_NAME;
#endif
  }

  void Node::set_publisher(std::function<void(int, AnyMsg)> pub_func) { publisher_ = pub_func; }

  void Node::set_connection_checker(std::function<bool(int)> check_func) { connection_checker_ = check_func; }

  void Node::on_input(int port, const AnyMsg &msg) {
    if (input_handlers_.find(port) != input_handlers_.end()) {
      input_handlers_[port](msg);
    }
  }

  void Node::update_parameter(const std::string &name, const std::string &value) {
    if (parameter_handlers_.find(name) != parameter_handlers_.end()) {
      try {
        parameter_handlers_[name](value);
      } catch (const std::exception &e) {
        FINS_LOG_ERROR("[Node {} Error] Update parameter '{}' failed: {}", meta_.name, name, e.what());
      }
    } else {
      FINS_LOG_WARN("[Node {} Warning] Unknown parameter: {}", meta_.name, name);
    }
  }

  std::vector<LogEntry> Node::get_logs() { return logger->get_and_clear_logs(); }

  NodeMeta Node::get_meta() const { return meta_; }

  ScopedSegmentTimer Node::recorder(const std::string& label, AcqTime acq_time) {
    return ScopedSegmentTimer(this->meta_.name, label, acq_time);
  }

  ScopedSegmentTimer Node::recorder(const std::string& label, double acq_time_sec) {
    return ScopedSegmentTimer(this->meta_.name, label, acq_time_sec);
  }

  void Node::set_client_topic(const std::string &key, const std::string &topic) { client_remaps_[key] = topic; }

  void Node::set_server_topic(const std::string &key, const std::string &topic) {
    server_remaps_[key] = topic;
    auto it = server_handles_.find(key);
    if (it != server_handles_.end()) {
      FINS_LOG_INFO("[Node] Applying Server Remap: Internal '{}' -> Topic '{}'", key, topic);
      if (it->second.handler) {
        FINS_SERVICE_MANAGER.register_service_handler(topic, std::move(it->second.handler), it->second.input_id, it->second.output_id);
      } else if (it->second.callback) {
        FINS_SERVICE_MANAGER.register_service(topic, it->second.callback, it->second.input_id, it->second.output_id);
      }
    }
  }

  void Node::set_commander_topic(const std::string &key, const std::string &topic) {
    commander_remaps_[key] = topic;
    auto it = commander_handles_.find(key);
    if (it != commander_handles_.end()) {
      FINS_LOG_INFO("[Node] Applying Commander Remap: Internal '{}' -> Topic '{}'", key, topic);
      FINS_ACTION_MANAGER.register_commander(topic, it->second.goal_type_id, it->second.feedback_type_id,
                                              it->second.result_callback, it->second.feedback_callback);
    }
  }

  void Node::set_actor_topic(const std::string &key, const std::string &topic) {
    actor_remaps_[key] = topic;
    auto it = actor_handles_.find(key);
    if (it != actor_handles_.end()) {
      FINS_LOG_INFO("[Node] Applying Actor Remap: Internal '{}' -> Topic '{}'", key, topic);
      FINS_ACTION_MANAGER.register_actor(topic, it->second.goal_type_id, it->second.feedback_type_id,
                                          it->second.goal_callback);
    }
  }

  void Node::set_name(const std::string &name) { meta_.name = name; }
  void Node::set_description(const std::string &desc) { meta_.description = desc; }
  void Node::set_category(const std::string &cat) { meta_.category = cat; }
  void Node::set_version(const std::string &ver) { meta_.version = ver; }
  void Node::set_basics(const std::string &name, const std::string &desc, const std::string &cat, const std::string &ver) {
    set_name(name);
    set_description(desc);
    set_category(cat);
    set_version(ver);
  }

  void Node::register_server_handle(const std::string &name, std::type_index in_id, std::type_index out_id, std::unique_ptr<ServiceHandler> handler) {
    server_handles_[name] = {nullptr, in_id, out_id, std::move(handler)};
  }

  void Node::register_commander_handle(const std::string &name, std::type_index goal_id, std::type_index feedback_id, 
                                       std::function<void(ActionState)> res_cb, std::function<void(const std::vector<std::any>&)> fb_cb) {
    commander_handles_[name] = {res_cb, fb_cb, goal_id, feedback_id};
    FINS_ACTION_MANAGER.register_commander(name, goal_id, feedback_id, res_cb, fb_cb);
  }

  void Node::register_actor_handle(const std::string &name, std::type_index goal_id, std::type_index feedback_id, 
                                   std::function<void(std::shared_ptr<ActionSessionBase>, const std::vector<std::any>&)> goal_cb) {
    actor_handles_[name] = {goal_cb, goal_id, feedback_id};
    FINS_ACTION_MANAGER.register_actor(name, goal_id, feedback_id, goal_cb);
  }

  std::shared_ptr<ActionSessionBase> Node::create_action_impl(const std::string &name, std::vector<std::any> args) {
    auto cmd_it = commander_handles_.find(name);
    if (cmd_it == commander_handles_.end()) {
      throw std::runtime_error("Commander '" + name + "' not registered");
    }
    std::string actual_topic = name;
    if (commander_remaps_.count(name)) {
      actual_topic = commander_remaps_[name];
    }
    return FINS_ACTION_MANAGER.create_action_session(actual_topic, std::move(args), cmd_it->second.goal_type_id, cmd_it->second.feedback_type_id);
  }

  ActionState Node::get_action_state(const std::string &name) {
    std::string actual_topic = name;
    if (commander_remaps_.count(name)) {
      actual_topic = commander_remaps_[name];
    }
    return FINS_ACTION_MANAGER.get_action_state(actual_topic);
  }

  void Node::cancel_action(const std::string &name) {
    std::string actual_topic = name;
    if (commander_remaps_.count(name)) {
      actual_topic = commander_remaps_[name];
    }
    FINS_ACTION_MANAGER.cancel_action(actual_topic);
  }

  // =========================================================================
  // NodeFactory Implementation
  // =========================================================================
  NodeFactory &NodeFactory::get_instance() {
    static NodeFactory instance;
    return instance;
  }

  void NodeFactory::register_node(const NodeMeta &meta, CreatorFunc creator) {
    std::string unique_name = meta.source + "/" + meta.name + "@" + meta.version;
    auto it = creators_.find(unique_name);
    if (it != creators_.end()) {
      creators_[unique_name] = creator;
      metas_[unique_name] = meta;
      FINS_LOG_DEBUG("[NodeFactory] Updated logic for existing node: {}", unique_name);
    } else {
      creators_[unique_name] = creator;
      metas_[unique_name] = meta;
      names_.push_back(unique_name);
      FINS_LOG_DEBUG("[NodeFactory] Registered new node: {}", unique_name);
    }
  }

  void NodeFactory::print_registered_nodes() {
    FINS_LOG_INFO("[NodeFactory] Registered Nodes:");
    for (const auto &name: names_) {
      FINS_LOG_INFO(" - {}", name);
    }
  }

  INode *NodeFactory::create(const std::string &name) {
    if (creators_.find(name) != creators_.end()) {
      INode *node = creators_[name]();
      node->define();
      return node;
    }
    return nullptr;
  }

  size_t NodeFactory::count() const { return names_.size(); }

  const char *NodeFactory::get_name(size_t index) const {
    if (index < names_.size())
      return names_[index].c_str();
    return nullptr;
  }

  std::string NodeFactory::get_json(const std::string &name) {
    if (metas_.find(name) != metas_.end()) {
      return metas_[name].to_json_string();
    }
    return "{}";
  }

  std::string NodeFactory::get_capabilities_json() const {
    nlohmann::json caps = nlohmann::json::object();
    for (const auto &pair: metas_) {
      caps[pair.first] = nlohmann::json::parse(pair.second.to_json_string());
    }
    return caps.dump();
  }

} // namespace fins