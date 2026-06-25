#pragma once
#include <string>

namespace fins {
  struct LogEntry {
    double timestamp;
    std::string level;
    std::string message;
    std::string file;
    uint32_t line;
  };
} // namespace fins