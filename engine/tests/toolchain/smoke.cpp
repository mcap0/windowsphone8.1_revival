// Toolchain smoke test: C++23 library features + threads + static CRT, linked only against the phone's DLLs.
#include <algorithm>
#include <atomic>
#include <expected>
#include <format>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

static std::expected<int, std::string> parse(std::string_view s)
{
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return std::unexpected(std::format("bad digit '{}'", c));
        v = v * 10 + (c - '0');
    }
    return v;
}

extern "C" __declspec(dllexport) int smoke_run(char* out, int capacity)
{
    std::vector<int> values { 5, 3, 9, 1 };
    std::ranges::sort(values);
    std::atomic<int> sum { 0 };
    std::mutex lock;
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.emplace_back([&, i] { std::lock_guard guard(lock); sum += values[i] * 100; });
    for (auto& t : threads)
        t.join();
    auto parsed = parse("1234");
    auto text = std::format("sorted={} sum={} parsed={} tick={}", values.front(), sum.load(), parsed.value_or(-1), GetTickCount64() > 0);
    auto n = std::min<size_t>(text.size(), capacity - 1);
    std::ranges::copy(std::span(text).first(n), out);
    out[n] = 0;
    return static_cast<int>(n);
}
