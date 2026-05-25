// Lab 3 — Clock Sweep buffer replacement
// Author: Nase Anishka (Roll No. 10075)
//
// A tiny templated cache that mimics the page-replacement policy used by
// PostgreSQL's shared_buffers. Each frame carries a reference bit; on a
// miss we walk a circular "clock hand" and either clear a hot frame's
// bit (giving it a second chance) or evict a cold frame (ref bit = 0).
//
// Build:   cmake -B build && cmake --build build
// Run:     ./build/clock_sweep

#include <iostream>
#include <iomanip>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

template <typename Key, typename Value>
class ClockSweepCache {
public:
    explicit ClockSweepCache(std::size_t capacity)
        : capacity_(capacity), frames_(capacity), hand_(0) {
        if (capacity_ == 0) {
            throw std::invalid_argument("capacity must be > 0");
        }
    }

    // Reads a key. On a hit the reference bit is set so the page survives
    // the next sweep. On a miss returns std::nullopt without mutating the
    // cache (a real DB would then fetch the page from disk and call put).
    std::optional<Value> get(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            log("MISS  get(" + to_str(key) + ")");
            return std::nullopt;
        }
        Frame& f = frames_[it->second];
        f.referenced = true;
        log("HIT   get(" + to_str(key) + ") -> " + to_str(f.value) +
            "   [frame " + std::to_string(it->second) + "]");
        return f.value;
    }

    // Inserts or updates a key. If the cache is full, runs the clock sweep
    // to pick a victim. Returns the index of the frame the key now lives in.
    std::size_t put(const Key& key, const Value& value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            Frame& f = frames_[it->second];
            f.value = value;
            f.referenced = true;
            log("UPDT  put(" + to_str(key) + ", " + to_str(value) +
                ")   [frame " + std::to_string(it->second) + "]");
            return it->second;
        }

        const std::size_t victim = pickVictim();
        Frame& f = frames_[victim];
        if (f.valid) {
            log("EVICT frame " + std::to_string(victim) + " -> drop key " +
                to_str(f.key));
            index_.erase(f.key);
        }
        f.key = key;
        f.value = value;
        f.valid = true;
        f.referenced = true;
        index_[key] = victim;
        log("LOAD  put(" + to_str(key) + ", " + to_str(value) +
            ")   [frame " + std::to_string(victim) + "]");
        // Advance past the freshly loaded frame so we don't immediately
        // reconsider it on the very next miss.
        hand_ = (victim + 1) % capacity_;
        return victim;
    }

    void dump(const std::string& label = "state") const {
        std::cout << "\n--- " << label << " ---\n";
        std::cout << std::left
                  << std::setw(7) << "frame"
                  << std::setw(8) << "valid"
                  << std::setw(6) << "ref"
                  << std::setw(10) << "key"
                  << std::setw(10) << "value"
                  << "hand\n";
        for (std::size_t i = 0; i < capacity_; ++i) {
            const Frame& f = frames_[i];
            std::cout << std::left
                      << std::setw(7) << i
                      << std::setw(8) << (f.valid ? "yes" : "-")
                      << std::setw(6) << (f.valid ? (f.referenced ? "1" : "0") : "-")
                      << std::setw(10) << (f.valid ? to_str(f.key) : "-")
                      << std::setw(10) << (f.valid ? to_str(f.value) : "-")
                      << (i == hand_ ? "<--" : "")
                      << '\n';
        }
        std::cout << '\n';
    }

    void setVerbose(bool v) { verbose_ = v; }

private:
    struct Frame {
        Key   key{};
        Value value{};
        bool  valid{false};
        bool  referenced{false};
    };

    // Walks the clock until it finds a frame whose reference bit is 0
    // (or an invalid/empty frame). Every full pass clears one round of
    // reference bits — this is the "second chance" mechanic.
    std::size_t pickVictim() {
        while (true) {
            Frame& f = frames_[hand_];
            if (!f.valid) {
                return hand_;
            }
            if (f.referenced) {
                f.referenced = false;
                log("SWEEP frame " + std::to_string(hand_) + " ref 1 -> 0");
            } else {
                return hand_;
            }
            hand_ = (hand_ + 1) % capacity_;
        }
    }

    void log(const std::string& msg) const {
        if (verbose_) std::cout << "  " << msg << '\n';
    }

    template <typename T>
    static std::string to_str(const T& v) {
        if constexpr (std::is_same_v<T, std::string>) return v;
        else return std::to_string(v);
    }

    std::size_t                       capacity_;
    std::vector<Frame>                frames_;
    std::unordered_map<Key, std::size_t> index_;
    std::size_t                       hand_;
    bool                              verbose_{true};
};

int main() {
    std::cout << "Clock-Sweep buffer cache demo (capacity = 4)\n";
    ClockSweepCache<int, std::string> cache(4);

    // ---- scenario 1: fill the cache, no evictions yet ----
    cache.put(101, "alice");
    cache.put(102, "bob");
    cache.put(103, "carol");
    cache.put(104, "dave");
    cache.dump("after initial 4 inserts");

    // ---- scenario 2: re-touch some keys so their ref bits become 1 ----
    cache.get(101);
    cache.get(103);
    cache.dump("after touching 101 and 103");

    // ---- scenario 3: insert a 5th key, force the sweep ----
    // Hand should be at frame 0 (key 101). 101 has ref=1, so the sweep
    // clears it and advances. 102 has ref=0 -> evicted. New key goes there.
    cache.put(105, "erin");
    cache.dump("after inserting key 105 (one eviction expected)");

    // ---- scenario 4: another insert evicts the next cold frame ----
    // Hand is parked at frame 1 (key 102, ref=0 after scenario 3's sweep).
    // No sweep needed -- 102 is evicted directly and 106 takes its slot.
    cache.put(106, "frank");
    cache.dump("after inserting key 106 (second eviction expected)");

    // ---- scenario 5: prove an updated key keeps its frame ----
    cache.put(105, "erin-updated");
    cache.dump("after updating key 105 in place");

    // ---- scenario 6: confirm evicted keys are gone ----
    auto v = cache.get(101);
    std::cout << "  lookup of evicted key 101 returned "
              << (v.has_value() ? *v : std::string("<miss>")) << "\n";

    return 0;
}
