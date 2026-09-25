#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>

namespace ninfer::serve {

// Fixed-bucket Prometheus-compatible histogram. Bucket upper bounds are finite
// and rendered as an explicit +Inf overflow bucket, matching the cumulative
// upper-bound-count semantics of /metrics so a scrape can derive mean and
// percentiles without further transforms. observe() is called from the request
// completion path and write_metric() from the /metrics handler, so the counters
// and running sum are atomics.
class PrometheusHistogram {
public:
    static constexpr std::size_t kMaxBuckets = 32;

    explicit PrometheusHistogram(std::initializer_list<double> bounds) {
        std::size_t n = 0;
        for (double b : bounds) {
            if (n >= kMaxBuckets) {
                break;
            }
            bounds_[n++] = b;
        }
        num_buckets_ = n;
    }

    void observe(double value) noexcept {
        std::size_t i = 0;
        while (i < num_buckets_ && value > bounds_[i]) {
            ++i;
        }
        counts_[i].fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(value, std::memory_order_relaxed);
    }

    // Appends the four text lines (# HELP, # TYPE, cumulative _bucket rows and
    // _sum/_count) for a metric named `name` to `out`.
    void write_metric(std::string& out, std::string_view name,
                      std::string_view help) const {
        char line[96];
        const int helper = std::snprintf(line, sizeof(line), "# HELP %.*s %.*s\n",
                                         static_cast<int>(name.size()), name.data(),
                                         static_cast<int>(help.size()), help.data());
        if (helper > 0) {
            out.append(line, static_cast<std::size_t>(helper));
        }
        const int type = std::snprintf(line, sizeof(line), "# TYPE %.*s histogram\n",
                                       static_cast<int>(name.size()), name.data());
        if (type > 0) {
            out.append(line, static_cast<std::size_t>(type));
        }
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < num_buckets_; ++i) {
            cumulative += counts_[i].load(std::memory_order_relaxed);
            const int row = std::snprintf(line, sizeof(line), "%.*s_bucket{le=\"%g\"} %llu\n",
                                          static_cast<int>(name.size()), name.data(),
                                          bounds_[i], static_cast<unsigned long long>(cumulative));
            if (row > 0) {
                out.append(line, static_cast<std::size_t>(row));
            }
        }
        cumulative += counts_[num_buckets_].load(std::memory_order_relaxed);
        const int inf = std::snprintf(line, sizeof(line), "%.*s_bucket{le=\"+Inf\"} %llu\n",
                                      static_cast<int>(name.size()), name.data(),
                                      static_cast<unsigned long long>(cumulative));
        if (inf > 0) {
            out.append(line, static_cast<std::size_t>(inf));
        }
        const int sum = std::snprintf(line, sizeof(line), "%.*s_sum %g\n",
                                      static_cast<int>(name.size()), name.data(),
                                      sum_.load(std::memory_order_relaxed));
        if (sum > 0) {
            out.append(line, static_cast<std::size_t>(sum));
        }
        const std::uint64_t total = total_count();
        const int count = std::snprintf(line, sizeof(line), "%.*s_count %llu\n",
                                        static_cast<int>(name.size()), name.data(),
                                        static_cast<unsigned long long>(total));
        if (count > 0) {
            out.append(line, static_cast<std::size_t>(count));
        }
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_count(); }
    [[nodiscard]] double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

private:
    [[nodiscard]] std::uint64_t total_count() const noexcept {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i <= num_buckets_; ++i) {
            total += counts_[i].load(std::memory_order_relaxed);
        }
        return total;
    }

    std::array<double, kMaxBuckets> bounds_{};
    std::array<std::atomic<std::uint64_t>, kMaxBuckets + 1> counts_{};
    std::atomic<double> sum_{0.0};
    std::size_t num_buckets_ = 0;
};

} // namespace ninfer::serve