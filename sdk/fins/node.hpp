/*******************************************************************************
 * Copyright (c) 2025.
 * IWIN-FINS Lab, Shanghai Jiao Tong University, Shanghai, China.
 * All rights reserved.
 ******************************************************************************/

// node.hpp

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <typeindex>

#include <fins/msg.hpp>
#include <fins/log_entry.hpp>
#include <fins/node_log.hpp>
#include <fins/service/service_manager.hpp>
#include <fins/service/service_tags.hpp>
#include <fins/service/service_traits.hpp>
#include <fins/action/action_tags.hpp>
#include <fins/action/action_traits.hpp>
#include <fins/action/action_manager.hpp>
#include <fins/type/string_convert.hpp>
#include <fins/type/type_register.hpp>
#include <fins/utils/time.hpp>
#include <fins/utils/performance_recorder.hpp>
#include <fins/server/parameter_server.hpp>

namespace fins {
  class NodeLogger; // 🌟 前置声明，避免在此处展开 fmt 模板
  class ScopedSegmentTimer;
  class ServiceHandler;
}

namespace fins {

  struct PortInfo {
    std::string name;
    std::string type;
  };

  struct ParameterInfo {
    std::string name;
    std::string type;
    std::string default_value;
  };

  struct ServiceInfo {
    std::string name;
    std::string request_type;
    std::string response_type;
  };

  struct ActionInfo {
    std::string name;
    std::string goal_type;
    std::string feedback_type;
  };

  enum class SchedulePriority {
    Urgent,
    High,
    Medium,
    Low
  };

  enum class ScheduleQueue {
    FCFS,
    LGFS
  };

  struct ScheduleInfo {
    SchedulePriority priority = SchedulePriority::Medium;
    ScheduleQueue queue = ScheduleQueue::FCFS;
  };

  struct NodeMeta {
    std::string name;
    std::string description;
    std::string category;
    std::string source;
    std::string package_name;
    std::string version = "default";

    std::vector<PortInfo> inputs;
    std::vector<PortInfo> outputs;
    std::vector<ParameterInfo> parameters;

    std::vector<ServiceInfo> clients;
    std::vector<ServiceInfo> servers;

    std::vector<ActionInfo> commanders;
    std::vector<ActionInfo> actors;

    ScheduleInfo schedule;

    std::string to_json_string() const;
  };

  class INode {
  public:
    virtual ~INode() = default;
    virtual void set_publisher(std::function<void(int, AnyMsg)> pub_func) = 0;
    virtual void set_connection_checker(std::function<bool(int)> check_func) = 0;

    virtual void define();
    virtual void initialize();
    virtual void run();
    virtual void pause(); 
    virtual void reset();
    
    virtual void on_input(int port, const AnyMsg &msg) = 0;
    virtual void update_parameter(const std::string &name, const std::string &value) = 0;
    virtual NodeMeta get_meta() const = 0;
    
    virtual ScopedSegmentTimer recorder(const std::string& label, AcqTime acq_time) = 0;
    virtual ScopedSegmentTimer recorder(const std::string& label, double acq_time_sec) = 0;
    virtual std::vector<LogEntry> get_logs() = 0;

    virtual void set_client_topic(const std::string &name, const std::string &topic) = 0;
    virtual void set_server_topic(const std::string &name, const std::string &topic) = 0;
    virtual void set_commander_topic(const std::string &name, const std::string &topic) = 0;
    virtual void set_actor_topic(const std::string &name, const std::string &topic) = 0;
  };

  template<typename T>
  struct member_func_traits;
  template<typename R, typename C, typename... Args>
  struct member_func_traits<R (C::*)(Args...)> {
    using class_type = C;
  };
  template<typename R, typename C, typename... Args>
  struct member_func_traits<R (C::*)(Args...) const> {
    using class_type = C;
  };

  template<typename InTuple, typename Ret>
  struct ClientGenerator;

  template<typename... InArgs, typename Ret>
  struct ClientGenerator<std::tuple<InArgs...>, Ret> {
    static auto generate(const std::string& name) {
      return ServiceClient<Ret, InArgs...>(name);
    }
  };

  template<typename Tuple, typename Ret, typename Class, typename Func>
  struct TypedServerBinder;

  template<typename... InArgs, typename Ret, typename Class, typename Func>
  struct TypedServerBinder<std::tuple<InArgs...>, Ret, Class, Func> {
    static void bind(const std::string& topic, Class* instance, Func func) {
      std::function<Ret(InArgs...)> bound_fn = [instance, func](InArgs... args) -> Ret {
        return (instance->*func)(std::forward<InArgs>(args)...);
      };
      FINS_SERVICE_MANAGER.register_typed_service<Ret, InArgs...>(topic, std::move(bound_fn));
    }
  };

  class Node : public INode {
  protected:
    NodeMeta meta_;
    std::map<int, std::function<void(const AnyMsg &)>> input_handlers_;
    std::map<std::string, std::function<void(const std::string &)>> parameter_handlers_;
    std::function<void(int, AnyMsg)> publisher_;
    std::function<bool(int)> connection_checker_;

    std::map<std::string, int> input_name_to_port_;
    std::map<std::string, int> output_name_to_port_;
    int next_input_port_ = 0;
    int next_output_port_ = 0;

    std::map<std::string, std::string> client_remaps_;

    struct ServerHandle {
      std::function<std::any(const std::vector<std::any> &)> callback; 
      std::type_index input_id = std::type_index(typeid(void));
      std::type_index output_id = std::type_index(typeid(void));
      std::unique_ptr<ServiceHandler> handler;
    };
    std::map<std::string, ServerHandle> server_handles_;
    std::map<std::string, std::string> server_remaps_;

    struct CommanderHandle {
      std::function<void(ActionState)> result_callback;
      std::function<void(const std::vector<std::any> &)> feedback_callback;
      std::type_index goal_type_id = std::type_index(typeid(void));
      std::type_index feedback_type_id = std::type_index(typeid(void));
    };
    std::map<std::string, CommanderHandle> commander_handles_;
    std::map<std::string, std::string> commander_remaps_;

    struct ActorHandle {
      std::function<void(std::shared_ptr<ActionSessionBase>, const std::vector<std::any> &)> goal_callback;
      std::type_index goal_type_id = std::type_index(typeid(void));
      std::type_index feedback_type_id = std::type_index(typeid(void));
    };
    std::map<std::string, ActorHandle> actor_handles_;
    std::map<std::string, std::string> actor_remaps_;

  public:
    std::shared_ptr<NodeLogger> logger;

    Node();
    void set_publisher(std::function<void(int, AnyMsg)> pub_func) override;
    void set_connection_checker(std::function<bool(int)> check_func) override;
    void on_input(int port, const AnyMsg &msg) override;
    void update_parameter(const std::string &name, const std::string &value) override;
    std::vector<LogEntry> get_logs() override;
    NodeMeta get_meta() const override;

    ScopedSegmentTimer recorder(const std::string& label, AcqTime acq_time) override;
    ScopedSegmentTimer recorder(const std::string& label, double acq_time_sec) override;

    void set_client_topic(const std::string &key, const std::string &topic) override;
    void set_server_topic(const std::string &key, const std::string &topic) override;
    void set_commander_topic(const std::string &key, const std::string &topic) override;
    void set_actor_topic(const std::string &key, const std::string &topic) override;

  protected:
    void set_name(const std::string &name);
    void set_description(const std::string &desc);
    void set_category(const std::string &cat);
    void set_version(const std::string &ver);
    void set_basics(const std::string &name, const std::string &desc, const std::string &cat, const std::string &ver = "default");

    template<int Port, typename T, typename ClassType>
    void register_input(const std::string &name, void (ClassType::*method)(const Msg<T> &)) {
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.inputs.size() <= static_cast<size_t>(Port))
        meta_.inputs.resize(Port + 1);
      meta_.inputs[Port] = {name, type_str};
      std::type_index expected_id = std::type_index(typeid(T));
      input_handlers_[Port] = [this, method, expected_id](const AnyMsg &any_msg) {
        if (any_msg.type_id != expected_id) return;
        Msg<T> typed_msg(any_msg);
        (static_cast<ClassType *>(this)->*method)(typed_msg);
      };
    }

    template<int Port, typename T, typename ClassType>
    void register_input(const std::string &name, void (ClassType::*method)(const T &, AcqTime)) {
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.inputs.size() <= static_cast<size_t>(Port))
        meta_.inputs.resize(Port + 1);
      meta_.inputs[Port] = {name, type_str};
      std::type_index expected_id = std::type_index(typeid(T));
      input_handlers_[Port] = [this, method, expected_id](const AnyMsg &any_msg) {
        if (any_msg.type_id != expected_id) return;
        Msg<T> typed_msg(any_msg);
        (static_cast<ClassType *>(this)->*method)(*typed_msg.data, typed_msg.acq_time);
      };
    }

    template<int Port, typename T, typename ClassType>
    void register_input(const std::string &name, void (ClassType::*method)(const T &)) {
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.inputs.size() <= static_cast<size_t>(Port))
        meta_.inputs.resize(Port + 1);
      meta_.inputs[Port] = {name, type_str};
      std::type_index expected_id = std::type_index(typeid(T));
      input_handlers_[Port] = [this, method, expected_id](const AnyMsg &any_msg) {
        if (any_msg.type_id != expected_id) return;
        Msg<T> typed_msg(any_msg);
        (static_cast<ClassType *>(this)->*method)(*typed_msg.data);
      };
    }

    template<typename T, typename ClassType>
    void register_input(const std::string &name, void (ClassType::*method)(const Msg<T> &)) {
      int port = next_input_port_++;
      input_name_to_port_[name] = port;
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.inputs.size() <= static_cast<size_t>(port))
        meta_.inputs.resize(port + 1);
      meta_.inputs[port] = {name, type_str};
      std::type_index expected_id = std::type_index(typeid(T));
      input_handlers_[port] = [this, method, expected_id](const AnyMsg &any_msg) {
        if (any_msg.type_id != expected_id) return;
        Msg<T> typed_msg(any_msg);
        (static_cast<ClassType *>(this)->*method)(typed_msg);
      };
    }

    template<typename T, typename ClassType>
    void register_input(const std::string &name, void (ClassType::*method)(const T &, AcqTime)) {
      int port = next_input_port_++;
      input_name_to_port_[name] = port;
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.inputs.size() <= static_cast<size_t>(port))
        meta_.inputs.resize(port + 1);
      meta_.inputs[port] = {name, type_str};
      std::type_index expected_id = std::type_index(typeid(T));
      input_handlers_[port] = [this, method, expected_id](const AnyMsg &any_msg) {
        if (any_msg.type_id != expected_id) return;
        Msg<T> typed_msg(any_msg);
        (static_cast<ClassType *>(this)->*method)(*typed_msg.data, typed_msg.acq_time);
      };
    }

    template<typename T, typename ClassType>
    void register_input(const std::string &name, void (ClassType::*method)(const T &)) {
      int port = next_input_port_++;
      input_name_to_port_[name] = port;
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.inputs.size() <= static_cast<size_t>(port))
        meta_.inputs.resize(port + 1);
      meta_.inputs[port] = {name, type_str};
      std::type_index expected_id = std::type_index(typeid(T));
      input_handlers_[port] = [this, method, expected_id](const AnyMsg &any_msg) {
        if (any_msg.type_id != expected_id) return;
        Msg<T> typed_msg(any_msg);
        (static_cast<ClassType *>(this)->*method)(*typed_msg.data);
      };
    }

    template<int Port, typename T>
    void register_output(const std::string &name) {
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.outputs.size() <= static_cast<size_t>(Port))
        meta_.outputs.resize(Port + 1);
      meta_.outputs[Port] = {name, type_str};
    }

    template<typename T>
    void register_output(const std::string &name) {
      int port = next_output_port_++;
      output_name_to_port_[name] = port;
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      if (meta_.outputs.size() <= static_cast<size_t>(port))
        meta_.outputs.resize(port + 1);
      meta_.outputs[port] = {name, type_str};
    }

    template<typename T>
    void register_parameter(const std::string &name, std::function<void(const T &)> handler) {
      parameter_handlers_[name] = [handler](const std::string &str_val) {
        T val = FINS_TYPE_REGISTER.string_convert<T>(str_val);
        handler(val);
      };
    }

    template<typename T, typename ClassType>
    void register_parameter(const std::string &name, void (ClassType::*method)(const T &), T default_value = T()) {
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      meta_.parameters.push_back({name, type_str, std::to_string(default_value)});
      parameter_handlers_[name] = [this, method](const std::string &str_val) {
        T val = FINS_TYPE_REGISTER.string_convert<T>(str_val);
        (static_cast<ClassType *>(this)->*method)(val);
      };
    }

    template<typename T, typename ClassType>
    void register_parameter(const std::string &name, void (ClassType::*method)(T), T default_value = T()) {
      std::string type_str = FINS_TYPE_REGISTER.get_name<T>();
      meta_.parameters.push_back({name, type_str, std::to_string(default_value)});
      parameter_handlers_[name] = [this, method](const std::string &str_val) {
        T val = FINS_TYPE_REGISTER.string_convert<T>(str_val);
        (static_cast<ClassType *>(this)->*method)(val);
      };
    }

  public:
    template<int Port, typename T>
    void send_ptr(std::shared_ptr<T> data, AcqTime ts = fins::now()) {
      if (publisher_) {
        AnyMsg msg(data, ts);
        publisher_(Port, msg);
      }
    }

    template<int Port, typename T>
    void send(const T &data, AcqTime ts = fins::now()) {
      if (publisher_) {
        auto data_ptr = std::make_shared<T>(data);
        AnyMsg msg(data_ptr, ts);
        publisher_(Port, msg);
      }
    }

    template<typename T>
    void send_ptr(const std::string &name, std::shared_ptr<T> data, AcqTime ts = fins::now()) {
      auto it = output_name_to_port_.find(name);
      if (it == output_name_to_port_.end()) return;
      if (publisher_) {
        AnyMsg msg(data, ts);
        publisher_(it->second, msg);
      }
    }

    template<typename T>
    void send(const std::string &name, const T &data, AcqTime ts = fins::now()) {
      auto it = output_name_to_port_.find(name);
      if (it == output_name_to_port_.end()) return;
      if (publisher_) {
        auto data_ptr = std::make_shared<T>(data);
        AnyMsg msg(data_ptr, ts);
        publisher_(it->second, msg);
      }
    }

    template<size_t N, typename T>
    void send_ptr(const char (&name)[N], std::shared_ptr<T> data, AcqTime ts = fins::now()) {
      send_ptr(std::string(name), data, ts);
    }

    template<size_t N, typename T>
    void send(const char (&name)[N], const T &data, AcqTime ts = fins::now()) {
      send(std::string(name), data, ts);
    }

    template<typename T>
    void send_dynamic(int port, std::shared_ptr<T> data, AcqTime ts = fins::now()) {
      if (publisher_) {
        AnyMsg msg(data, ts);
        publisher_(port, msg);
      }
    }

    template<int Port>
    bool required() {
      if (connection_checker_) {
        return connection_checker_(Port);
      }
      return false;
    }

    bool required(const std::string &name) {
      auto it = output_name_to_port_.find(name);
      if (it != output_name_to_port_.end() && connection_checker_) {
        return connection_checker_(it->second);
      }
      return false;
    }

    template<size_t N>
    bool required(const char (&name)[N]) {
      return required(std::string(name));
    }

  protected:
    template<typename Tuple, std::size_t... Is>
    std::string tuple_types_to_string_impl(std::index_sequence<Is...>) {
      std::stringstream ss;
      ((ss << (Is == 0 ? "" : ", ") << FINS_TYPE_REGISTER.get_name<std::tuple_element_t<Is, Tuple>>()), ...);
      return ss.str();
    }

    template<typename Tuple>
    std::string tuple_types_to_string() {
      return tuple_types_to_string_impl<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    }

    template<typename... Args>
    auto register_client(const std::string &name) {
      using Traits = ServiceTraits<Args...>;
      using InTuple = typename Traits::InputTuple;
      using OutTuple = typename Traits::OutputTuple;
      using RetType = typename Traits::ReturnType;

      std::string req_str = tuple_types_to_string<InTuple>();
      std::string res_str = tuple_types_to_string<OutTuple>();

      meta_.clients.push_back({name, req_str, res_str});

      std::string actual_topic = name;
      if (client_remaps_.count(name)) {
        actual_topic = client_remaps_[name];
      }

      return ClientGenerator<InTuple, RetType>::generate(actual_topic);
    }

    template<typename... Args, typename Func>
    void register_server(const std::string &name, Func &&callback_ptr) {
      using Traits = ServiceTraits<Args...>;
      using InTuple = typename Traits::InputTuple;
      using OutTuple = typename Traits::OutputTuple;
      using RetType = typename Traits::ReturnType;
      using ClassType = typename member_func_traits<std::decay_t<Func>>::class_type;

      std::string req_str = tuple_types_to_string<InTuple>();
      std::string res_str = tuple_types_to_string<OutTuple>();

      meta_.servers.push_back({name, req_str, res_str});

      std::string actual_topic = name;
      if (server_remaps_.count(name)) {
        actual_topic = server_remaps_[name];
      }

      TypedServerBinder<InTuple, RetType, ClassType, std::decay_t<Func>>::bind(
          actual_topic, static_cast<ClassType*>(this), callback_ptr
      );

      class TypedServiceHandler : public ServiceHandler {
          ClassType* instance_;
          std::decay_t<Func> func_;
      public:
          TypedServiceHandler(ClassType* inst, Func f) : instance_(inst), func_(f) {}
          std::any invoke(const std::any* args, size_t count) override {
              return call_member_array_impl<InTuple, RetType, ClassType>(
                  func_, instance_, args, std::make_index_sequence<std::tuple_size_v<InTuple>>{}
              );
          }
      };

      auto handler = std::make_unique<TypedServiceHandler>(static_cast<ClassType*>(this), callback_ptr);
      register_server_handle(name, std::type_index(typeid(InTuple)), std::type_index(typeid(OutTuple)), std::move(handler));
    }

    void register_server_handle(const std::string &name, std::type_index in_id, std::type_index out_id, std::unique_ptr<ServiceHandler> handler);

    template<typename... Args, typename ResultFunc, typename FeedbackFunc>
    void register_commander(const std::string &name, ResultFunc &&result_callback, FeedbackFunc &&feedback_callback) {
      using Traits = ActionTraits<Args...>;
      using GoalTuple = typename Traits::GoalTuple;
      using FeedbackTuple = typename Traits::FeedbackTuple;
      using ResultClassType = typename member_func_traits<std::decay_t<ResultFunc>>::class_type;
      using FeedbackClassType = typename member_func_traits<std::decay_t<FeedbackFunc>>::class_type;

      std::string goal_str = tuple_types_to_string<GoalTuple>();
      std::string feedback_str = tuple_types_to_string<FeedbackTuple>();

      meta_.commanders.push_back({name, goal_str, feedback_str});

      auto result_wrapper = [this, func = result_callback](ActionState state) {
        (static_cast<ResultClassType *>(this)->*func)(state);
      };

      auto feedback_wrapper = [this, func = feedback_callback](const std::vector<std::any> &args) {
        call_feedback_impl<FeedbackTuple, FeedbackClassType>(func, args, std::make_index_sequence<std::tuple_size_v<FeedbackTuple>>{});
      };

      register_commander_handle(name, std::type_index(typeid(GoalTuple)), std::type_index(typeid(FeedbackTuple)), result_wrapper, feedback_wrapper);
    }

    void register_commander_handle(const std::string &name, std::type_index goal_id, std::type_index feedback_id, 
                                   std::function<void(ActionState)> res_cb, std::function<void(const std::vector<std::any>&)> fb_cb);

    template<typename... Args, typename GoalFunc>
    void register_actor(const std::string &name, GoalFunc &&goal_callback) {
      using Traits = ActionTraits<Args...>;
      using GoalTuple = typename Traits::GoalTuple;
      using FeedbackTuple = typename Traits::FeedbackTuple;
      using GoalClassType = typename member_func_traits<std::decay_t<GoalFunc>>::class_type;

      std::string goal_str = tuple_types_to_string<GoalTuple>();
      std::string feedback_str = tuple_types_to_string<FeedbackTuple>();

      meta_.actors.push_back({name, goal_str, feedback_str});

      auto goal_wrapper = [this, func = goal_callback](std::shared_ptr<ActionSessionBase> session, const std::vector<std::any> &args) {
        call_goal_impl_with_session<GoalTuple, GoalClassType>(session, func, args, std::make_index_sequence<std::tuple_size_v<GoalTuple>>{});
      };

      register_actor_handle(name, std::type_index(typeid(GoalTuple)), std::type_index(typeid(FeedbackTuple)), goal_wrapper);
    }

    void register_actor_handle(const std::string &name, std::type_index goal_id, std::type_index feedback_id, 
                               std::function<void(std::shared_ptr<ActionSessionBase>, const std::vector<std::any>&)> goal_cb);

    template<typename... GoalArgs>
    std::shared_ptr<ActionSessionBase> create_action(const std::string &name, GoalArgs &&...goal_args) {
      std::vector<std::any> type_erased_args;
      (type_erased_args.push_back(std::any(std::forward<GoalArgs>(goal_args))), ...);
      return create_action_impl(name, std::move(type_erased_args));
    }

    template<typename... GoalArgs>
    std::shared_ptr<ActionSessionBase> create_action(const std::string &name, const GoalArgs &...goal_args) {
      std::vector<std::any> type_erased_args;
      (type_erased_args.push_back(std::any(goal_args)), ...);
      return create_action_impl(name, std::move(type_erased_args));
    }

    std::shared_ptr<ActionSessionBase> create_action_impl(const std::string &name, std::vector<std::any> args);
    ActionState get_action_state(const std::string &name);
    void cancel_action(const std::string &name);

  private:
    template<typename InTuple, typename RetType, typename ClassType, typename Func, size_t... Is>
    static std::any call_member_array_impl(Func func, ClassType* inst, const std::any* args, std::index_sequence<Is...>) {
        if constexpr (std::is_void_v<RetType>) {
            (inst->*func)(std::any_cast<std::tuple_element_t<Is, InTuple>>(args[Is])...);
            return std::any();
        } else {
            return std::any((inst->*func)(std::any_cast<std::tuple_element_t<Is, InTuple>>(args[Is])...));
        }
    }

    template<typename FeedbackTuple, typename ClassType, typename Func, size_t... Is>
    void call_feedback_impl(Func func, const std::vector<std::any> &args, std::index_sequence<Is...>) {
      auto typed_args = std::make_tuple(std::any_cast<std::tuple_element_t<Is, FeedbackTuple>>(args[Is])...);
      (static_cast<ClassType *>(this)->*func)(std::get<Is>(typed_args)...);
    }

    template<typename GoalTuple, typename ClassType, typename Func, size_t... Is>
    void call_goal_impl_with_session(std::shared_ptr<ActionSessionBase> session, Func func, const std::vector<std::any> &args, std::index_sequence<Is...>) {
      auto typed_args = std::make_tuple(std::any_cast<std::tuple_element_t<Is, GoalTuple>>(args[Is])...);
      (static_cast<ClassType *>(this)->*func)(session, std::get<Is>(typed_args)...);
    }
  };

  class NodeFactory {
  public:
    using CreatorFunc = std::function<INode *()>;

    static NodeFactory &get_instance();
    void register_node(const NodeMeta &meta, CreatorFunc creator);
    void print_registered_nodes();
    INode *create(const std::string &name);
    size_t count() const;
    const char *get_name(size_t index) const;
    std::string get_json(const std::string &name);
    std::string get_capabilities_json() const;

  private:
    std::map<std::string, CreatorFunc> creators_;
    std::map<std::string, NodeMeta> metas_;
    std::vector<std::string> names_;
    NodeFactory() = default;
  };

#define FINS_NODE_FACTORY fins::NodeFactory::get_instance()

#define EXPORT_NODE(UserClass)                                                                                    \
  namespace {                                                                                                     \
    struct Register_##UserClass {                                                                                 \
      Register_##UserClass() {                                                                                    \
        auto temp_ptr = std::make_unique<UserClass>();                                                            \
        temp_ptr->define();                                                                                       \
        fins::NodeMeta meta = temp_ptr->get_meta();                                                               \
        fins::NodeFactory::get_instance().register_node(meta, []() -> fins::INode * { return new UserClass(); }); \
      }                                                                                                           \
    };                                                                                                            \
    static Register_##UserClass register_inst_##UserClass;                                                        \
  }

  enum PluginState {
    STATEFUL,
    STATELESS
  };

#ifndef FINS_STATIC_BUILD
#define DEFINE_PLUGIN_ENTRY(state)                                                                      \
  extern "C" {                                                                                          \
  int get_node_count() { return static_cast<int>(fins::NodeFactory::get_instance().count()); }          \
  const char *get_node_name(int index) { return fins::NodeFactory::get_instance().get_name(index); }    \
  const char *get_node_meta_json(const char *name) {                                                    \
    static thread_local std::string json_buffer;                                                        \
    json_buffer = fins::NodeFactory::get_instance().get_json(name);                                     \
    return json_buffer.c_str();                                                                         \
  }                                                                                                     \
  fins::INode *create_node(const char *name) { return fins::NodeFactory::get_instance().create(name); } \
  void destroy_node(fins::INode *p) { delete p; }                                                       \
  void plugin_init();                                                                                   \
  void plugin_destroy();                                                                                \
  bool is_hot_reloadable() { return (state) == fins::STATELESS; }                                       \
  }
#else
#define DEFINE_PLUGIN_ENTRY(state)
#endif

#ifndef FINS_STATIC_BUILD
#define REGISTER_PLUGIN_INIT(CodeBlock) \
  extern "C" {                          \
  void plugin_init() { CodeBlock }      \
  }
#define REGISTER_PLUGIN_DESTROY(CodeBlock) \
  extern "C" {                             \
  void plugin_destroy() { CodeBlock }      \
  }
#else
#define REGISTER_PLUGIN_INIT(CodeBlock)                  \
  namespace {                                            \
    struct StaticPluginInit {                            \
      StaticPluginInit() { CodeBlock }                   \
    };                                                   \
    static StaticPluginInit static_plugin_init_instance; \
  }
#define REGISTER_PLUGIN_DESTROY(CodeBlock)                  \
  namespace {                                               \
    struct StaticPluginDestroy {                            \
      ~StaticPluginDestroy() { CodeBlock }                  \
    };                                                      \
    static StaticPluginDestroy static_plugin_dest_instance; \
  }
#endif

} // namespace fins