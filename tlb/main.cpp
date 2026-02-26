#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <getopt.h>
#include <cmath>
#include <thread>

using steady_clock = std::chrono::steady_clock;

static uint64_t median_vec(std::vector<uint64_t> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n == 0) return 0;
    if (n % 2 == 1) return v[n/2];
    return (v[n/2 - 1] + v[n/2]) / 2;
}

int main() {
    const size_t MAX_PAGES = 1 << 16;
    uint64_t TARGET_TOTAL_ACCESSES = 50'000'000ULL;
    int trials = 3;

    const size_t L1_FINE_START = 64;
    const size_t L1_FINE_END   = 128;
    const size_t L1_FINE_STEP  = 1;
    const double L1_THRESHOLD  = 0.20; 

    const size_t L2_FINE_START = 512;
    const size_t L2_FINE_END   = 2048;
    const size_t L2_FINE_STEP  = 32;
    const double L2_THRESHOLD  = 0.30; 

    static struct option long_options[] = {
        {"accesses", required_argument, 0, 'a'},
        {"trials", required_argument, 0, 't'},
        {0,0,0,0}
    };

    const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    std::cout << "Detected page size: " << page_size << " bytes\n";

    const size_t max_bytes = MAX_PAGES * page_size;
    void* raw = nullptr;
    if (posix_memalign(&raw, page_size, max_bytes) != 0 || raw == nullptr) {
        std::cerr << "Allocation failed for " << max_bytes << " bytes\n";
        return 1;
    }
    uint8_t* buf = static_cast<uint8_t*>(raw);

    // Touch pages to ensure mapping
    for (size_t off = 0; off < max_bytes; off += page_size) {
        buf[off] = static_cast<uint8_t>((off >> 12) & 0xFF);
    }

    std::mt19937_64 rng(1234567);

    // Measure function: returns avg ns per access (median over trials)
    auto measure_avg_ns = [&](size_t num_pages) -> double {
        std::vector<size_t> order(num_pages);
        for (size_t i = 0; i < num_pages; ++i) order[i] = i;

        size_t repeats = static_cast<size_t>(std::max<uint64_t>(1ULL, TARGET_TOTAL_ACCESSES / num_pages));
        uint64_t total_accesses = static_cast<uint64_t>(repeats) * static_cast<uint64_t>(num_pages);

        std::vector<uint64_t> trial_ns;
        trial_ns.reserve(trials);
        volatile uint64_t sink = 0;

        for (int tr = 0; tr < trials; ++tr) {
            std::shuffle(order.begin(), order.end(), rng);

            // compiler barrier
            asm volatile("" ::: "memory");

            auto t0 = steady_clock::now();
            for (size_t r = 0; r < repeats; ++r) {
                for (size_t i = 0; i < num_pages; ++i) {
                    size_t off = order[i] * page_size;
                    sink += buf[off];
                }
            }
            auto t1 = steady_clock::now();
            uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            trial_ns.push_back(ns);

            // small pause
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        uint64_t med_ns = median_vec(trial_ns);
        double avg_ns_per_access = static_cast<double>(med_ns) / static_cast<double>(total_accesses);
        return avg_ns_per_access;
    };

    // --- Coarse pass (powers of two) ---
    std::cout << "\n=== Coarse pass (powers of two) ===\n";
    std::vector<std::pair<size_t,double>> coarse;
    for (size_t num_pages = 1; num_pages <= MAX_PAGES; num_pages *= 2) {
        double avg = measure_avg_ns(num_pages);
        coarse.emplace_back(num_pages, avg);
        std::cout << "pages=" << num_pages << "  avg_ns=" << avg << "\n";
        rng.seed(rng() ^ (num_pages + 0x9e3779b97f4a7c15ULL));
    }

    std::vector<double> smalls;
    for (auto &p : coarse) if (p.first <= 64) smalls.push_back(p.second);
    double baseline = 0.0;
    if (!smalls.empty()) {
        std::sort(smalls.begin(), smalls.end());
        baseline = smalls[smalls.size()/2];
    } else baseline = coarse.front().second;
    std::cout << "\nBaseline (median for pages<=64) = " << baseline << " ns\n";

    std::cout << "\n=== Fine pass for L1 (pages " << L1_FINE_START << " .. " << L1_FINE_END << ") ===\n";
    size_t detected_l1 = 0;
    for (size_t num_pages = L1_FINE_START; num_pages <= L1_FINE_END; num_pages += L1_FINE_STEP) {
        double avg = measure_avg_ns(num_pages);
        std::cout << "pages=" << num_pages << "  avg_ns=" << avg;
        if (!detected_l1 && avg > baseline * (1.0 + L1_THRESHOLD)) {
            detected_l1 = num_pages;
            std::cout << "   <-- exceeds baseline by " << (avg / baseline - 1.0) * 100.0 << " %";
        }
        std::cout << "\n";
        rng.seed(rng() ^ (num_pages + 0x9e3779b97f4a7c15ULL));
    }
    if (detected_l1) {
        std::cout << "\nEstimated L1 boundary = " << detected_l1
                  << " pages -> estimated L1 dTLB size ≈ " << detected_l1 << " entries (approx)\n";
    } else {
        std::cout << "\nNo clear L1 boundary found in " << L1_FINE_START << ".."<<L1_FINE_END << "\n";
    }

    std::cout << "\n=== Fine pass for L2 (pages " << L2_FINE_START << " .. " << L2_FINE_END << " step " << L2_FINE_STEP << ") ===\n";
    size_t detected_l2 = 0;
    for (size_t num_pages = L2_FINE_START; num_pages <= L2_FINE_END; num_pages += L2_FINE_STEP) {
        double avg = measure_avg_ns(num_pages);
        std::cout << "pages=" << num_pages << "  avg_ns=" << avg;
        if (!detected_l2 && avg > baseline * (1.0 + L2_THRESHOLD)) {
            detected_l2 = num_pages;
            std::cout << "   <-- exceeds baseline by " << (avg / baseline - 1.0) * 100.0 << " %";
        }
        std::cout << "\n";
        rng.seed(rng() ^ (num_pages + 0x9e3779b97f4a7c15ULL));
    }
    if (detected_l2) {
        std::cout << "\nEstimated L2 boundary = " << detected_l2
                  << " pages -> estimated L2 TLB size ≈ " << detected_l2 << " entries (approx)\n";
    } else {
        std::cout << "\nNo clear L2 boundary found in " << L2_FINE_START << ".."<<L2_FINE_END << "\n";
    }

    std::cout << "\n=== Summary ===\n";
    std::cout << "Baseline (<=64 pages) = " << baseline << " ns\n";
    if (detected_l1) std::cout << "Detected L1 approx = " << detected_l1 << " pages\n";
    else std::cout << "Detected L1 = not found\n";
    if (detected_l2) std::cout << "Detected L2 approx = " << detected_l2 << " pages\n";
    else std::cout << "Detected L2 = not found\n";

    free(raw);
    return 0;
}
