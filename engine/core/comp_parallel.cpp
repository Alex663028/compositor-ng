// AI生成
#include "comp_parallel.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<size_t> g_threads{0};

/* band 调度线程数：默认 1（顺序），与 M1 行为一致。 */
std::atomic<size_t> g_band_threads{1};

size_t hardware_threads() {
  unsigned int hc = std::thread::hardware_concurrency();
  return hc == 0 ? static_cast<size_t>(4) : static_cast<size_t>(hc);
}

size_t configured_threads() {
  size_t n = g_threads.load(std::memory_order_relaxed);
  return n == 0 ? hardware_threads() : n;
}

/*
 * 区间回调的异常收集槽。
 *
 * 回调虽然是 C 函数指针，实际实现常是 C++（composite / filter_graph 等），
 * 因此仍需兜住 bad_alloc 一类的意外异常。多个工作线程会并发写入「首个异常」，
 * 必须用互斥量串行化——原实现是对 std::exception_ptr 的裸赋值，属数据竞态。
 */
class ErrorSlot {
 public:
  /* 只记录首个异常；必须在 catch 处理器内调用。 */
  void capture() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_ == nullptr) error_ = std::current_exception();
    } catch (...) {
      /* 加锁失败极不可能；此处一律不再抛，避免在 catch 处理器中 terminate。 */
    }
    count_.fetch_add(1, std::memory_order_relaxed);
  }

  std::exception_ptr first() {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

  size_t count() const { return count_.load(std::memory_order_relaxed); }

 private:
  std::mutex mutex_;
  std::exception_ptr error_;
  std::atomic<size_t> count_{0};
};

/*
 * 最近一次调度失败的描述。线程局部：多个调用线程各自持有自己的诊断，
 * 不会像静态全局字符串那样互相覆盖，也就不会出现悬垂指针。
 */
thread_local std::string g_last_error;

/* 在本地把异常转成文本，绝不越过 extern "C" 边界。 */
std::string describe(const std::exception_ptr& error) {
  if (error == nullptr) return std::string();
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& e) {
    return std::string(e.what());
  } catch (...) {
    return std::string("unknown exception");
  }
}

void invoke_range(ErrorSlot& slot, comp_range_fn body, void* ctx, size_t begin, size_t end) {
  try {
    body(ctx, begin, end);
  } catch (...) {
    slot.capture();
  }
}

/*
 * 汇总诊断：无失败时清空上一条消息，避免把历史失败当成本次结果。
 * 赋值 std::string 可能抛 bad_alloc，而本函数位于「契约不抛异常」的路径上，
 * 故整体兜住。
 */
void publish_error(ErrorSlot& slot) noexcept {
  const bool failed = slot.count() != 0;
  try {
    if (!failed) {
      g_last_error.clear();
      return;
    }
    g_last_error = describe(slot.first());
    if (g_last_error.empty()) g_last_error = "range callback threw a non-standard exception";
  } catch (...) {
    /* 诊断本身失败不影响主流程（主流程的异常已在此前收集完毕）。 */
  }
}

/*
 * 把 [0, count) 切成至多 workers 个互不重叠、并集覆盖全区的连续区间。
 * 除最后一段外的所有区间都交给新建线程，最后一段由调用线程执行（少一次线程创建）；
 * 返回前 join 所有线程，因此回调可以安全地读取调用者栈帧上的对象（见 comp_bands）。
 *
 * 线程创建失败时该区间（及其后所有区间）退化为在调用线程上执行——契约要求
 * 「调度失败时退化为串行执行，不抛异常、不返回错误」。这里不能让异常逃出：
 * 若 std::thread 构造抛错而未被捕获，已创建的 joinable 线程在 vector 析构时
 * 会触发 std::terminate。
 */
void run_ranges_parallel(size_t count, comp_range_fn body, void* ctx, size_t workers) {
  const size_t chunk = (count + workers - 1) / workers;
  std::vector<std::thread> pool;
  pool.reserve(workers - 1); /* 唯一可能抛出的点，且发生在任何区间执行之前。 */
  ErrorSlot slot;
  bool degraded = false;

  for (size_t i = 0; i + 1 < workers; ++i) {
    const size_t begin = i * chunk;
    if (begin >= count) break;
    const size_t end = std::min(count, begin + chunk);
    if (!degraded) {
      try {
        pool.emplace_back([&, begin, end]() { invoke_range(slot, body, ctx, begin, end); });
        continue;
      } catch (...) {
        degraded = true;
      }
    }
    invoke_range(slot, body, ctx, begin, end);
  }

  /* 最后一段由调用线程执行。 */
  const size_t tail_begin = (workers - 1) * chunk;
  if (tail_begin < count) invoke_range(slot, body, ctx, tail_begin, count);

  for (std::thread& t : pool) {
    if (t.joinable()) t.join();
  }

  publish_error(slot);
}

void run_ranges(size_t count, comp_range_fn body, void* ctx, size_t workers) {
  if (workers < 1) workers = 1;
  if (workers > count) workers = count;

  /* 单线程：直接串行执行，保证确定性路径（回归测试使用）。 */
  if (workers <= 1) {
    ErrorSlot slot;
    invoke_range(slot, body, ctx, 0, count);
    publish_error(slot);
    return;
  }

  try {
    run_ranges_parallel(count, body, ctx, workers);
  } catch (...) {
    /*
     * 走到这里只可能是线程池分配失败（见 run_ranges_parallel 的注释），
     * 此时尚未执行任何区间，因此整体串行重跑不会重复执行回调。
     */
    ErrorSlot slot;
    invoke_range(slot, body, ctx, 0, count);
    publish_error(slot);
  }
}

}  // namespace

extern "C" size_t comp_parallel_threads(void) {
  return std::max<size_t>(1, configured_threads());
}

extern "C" size_t comp_parallel_set_threads(size_t thread_count) {
  g_threads.store(thread_count, std::memory_order_relaxed);
  return comp_parallel_threads();
}

extern "C" size_t comp_parallel_band_threads(void) {
  const size_t n = g_band_threads.load(std::memory_order_relaxed);
  return std::max<size_t>(1, n == 0 ? hardware_threads() : n);
}

extern "C" size_t comp_parallel_set_band_threads(size_t thread_count) {
  g_band_threads.store(thread_count, std::memory_order_relaxed);
  return comp_parallel_band_threads();
}

extern "C" void comp_parallel_ranges_n(size_t count, comp_range_fn body, void* ctx, size_t threads) {
  if (body == nullptr || count == 0) {
    g_last_error.clear();
    return;
  }
  run_ranges(count, body, ctx, threads);
}

extern "C" void comp_parallel_ranges(size_t count, comp_range_fn body, void* ctx) {
  if (body == nullptr || count == 0) {
    g_last_error.clear();
    return;
  }
  run_ranges(count, body, ctx, configured_threads());
}

extern "C" const char* comp_parallel_last_error(void) {
  /* OPT-5：与 comp_engine_last_error 统一——无错误返回空串，不返回 nullptr。
   * 两种「无错误」表达曾并存（"" vs NULL），宿主判空口径只能猜。 */
  return g_last_error.c_str();
}
