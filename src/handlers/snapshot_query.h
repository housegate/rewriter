#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace snapshot_query {
// A request owns one budget across parsing, binding, scratch work and every
// generation. The observer runs after real generation, never in its place.
struct Execution {
  std::function<bool()> cancelled;
  std::function<void()> after_generation;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};
} // namespace snapshot_query
