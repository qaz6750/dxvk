#pragma once
#include <windows.h>
#include <dxgi.h>
#include <functional>
#include <mutex>
#include <utility>

namespace dxvk::umd {
// Turnip owns its kernel device. Only the calling DDI needs the runtime
// callbacks. Keep nested resource retirements alive through the outer call.
class RuntimeService {
public:
  struct Retirement {
    Retirement* next = nullptr;
    virtual ~Retirement() = default;
    virtual void release() noexcept = 0;
  };
  struct Scope {
    RuntimeService* service;
    Scope* previous;
    bool amortized = false;
    inline static thread_local Scope* current = nullptr;
    explicit Scope(RuntimeService* value) : service(value), previous(current) { current = this; }
    ~Scope() { current = previous; }
  };
  bool isCaller() const {
    for (auto s = Scope::current; s; s = s->previous)
      if (s->service == this) return true;
    return false;
  }
  template<typename F> HRESULT invoke(F&& f) {
    if (!isCaller() || closed) return DXGI_ERROR_DEVICE_REMOVED;
    return f();
  }
  template<typename F> decltype(auto) run(F&& f) {
    std::lock_guard guard(mutex);
    Scope scope(this);
    ++depth;
    struct Leave {
      RuntimeService& service;
      ~Leave() { service.leave(); }
    } leave{*this};
    return f();
  }
  template<typename F> void drain(F&& f) { run(std::forward<F>(f)); }
  void retire(Retirement* node) {
    std::lock_guard guard(mutex);
    *tail = node;
    tail = &node->next;
    ++epoch;
  }
  uint64_t retirementEpoch() { std::lock_guard guard(mutex); return epoch; }
  void setAmortizedProcessing(std::function<void()> f) { amortized = std::move(f); }
  void requestAmortizedProcessing() { pending = true; }
  void close() { std::lock_guard guard(mutex); closed = true; pending = false; }
private:
  void releaseRetired() noexcept {
    while (head) {
      auto node = head;
      head = node->next;
      if (!head) tail = &head;
      node->release();
      delete node;
    }
  }
  void leave() noexcept {
    if (depth == 1) {
      releaseRetired();
      Scope* outer = nullptr;
      for (auto s = Scope::current; s; s = s->previous)
        if (s->service == this) outer = s;
      if (outer && !outer->amortized && !closed && pending && amortized) {
        outer->amortized = true;
        pending = false;
        amortized();
        releaseRetired();
      }
    }
    --depth;
  }
  std::recursive_mutex mutex;
  Retirement* head = nullptr;
  Retirement** tail = &head;
  std::function<void()> amortized;
  uint64_t epoch = 0;
  unsigned depth = 0;
  bool closed = false, pending = false;
};
}
