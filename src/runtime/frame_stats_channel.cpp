// ── FrameStatsChannel — implementation ───────────────────────────────────────
// See frame_stats_channel.h for what the three numbers mean and why the split
// between them is the point. Everything here runs at REPORT time except
// begin/endFrame, which are two clock reads and one sample scan.
#include "runtime/frame_stats_channel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

struct Stats {
    double min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
    bool   valid = false;
};

// Linear interpolation between the two neighbouring order statistics. With a
// few thousand samples the difference from a nearest-rank percentile is
// cosmetic, but interpolating means p99.9 does not simply report the max the
// moment the sample count drops below 1000.
double pct(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    const double pos = q * (double)(sorted.size() - 1);
    const size_t lo  = (size_t)pos;
    const size_t hi  = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - (double)lo);
}

Stats statsOf(std::vector<double> ms) {   // by value: sorted in place
    Stats s;
    if (ms.empty()) return s;
    std::sort(ms.begin(), ms.end());
    double sum = 0.0;
    for (double v : ms) sum += v;
    s.min   = ms.front();
    s.max   = ms.back();
    s.mean  = sum / (double)ms.size();
    s.p50   = pct(ms, 0.50);
    s.p90   = pct(ms, 0.90);
    s.p99   = pct(ms, 0.99);
    s.p999  = pct(ms, 0.999);
    s.valid = true;
    return s;
}

void printRow(const char* label, const Stats& s) {
    std::printf("             %-8s p50 %7.2f  p90 %7.2f  p99 %7.2f  "
                "p99.9 %7.2f  max %7.2f ms\n",
                label, s.p50, s.p90, s.p99, s.p999, s.max);
}

constexpr double kBinMs   = 0.5;   // histogram resolution
constexpr int    kBarCols = 40;

} // namespace

// ── Per-frame recording ─────────────────────────────────────────────────────

void FrameStatsChannel::beginFrame() {
    const uint64_t t = prof::nowNs();
    m_intervalNs = m_havePrev
        ? (uint32_t)std::min<uint64_t>(t - m_prevBegin, UINT32_MAX)
        : 0;
    m_prevBegin = t;
    m_beginNs   = t;
    m_havePrev  = true;
}

void FrameStatsChannel::endFrame() {
    // No cadence exists until a second frame has begun — the first frame would
    // otherwise record the whole of boot as its interval.
    if (!m_intervalNs) return;
    const uint64_t t = prof::nowNs();
    Sample s;
    s.intervalNs = m_intervalNs;
    s.cpuNs      = (uint32_t)std::min<uint64_t>(t - m_beginNs, UINT32_MAX);
    s.presentNs  = scopeNs("bgfx.frame");
    m_ring[(size_t)(m_total++ % kCapacity)] = s;
}

uint32_t FrameStatsChannel::scopeNs(const char* name) {
    // The hub runs the timer channel's endFrame BEFORE the extra channels', so
    // this frame's snapshot is already merged and readable here.
    for (const prof::TimerSample& s : prof::Profiler::get().timer().lastFrame()) {
        if (s.threadIndex != 0 || s.end < s.start) continue;   // end==0: unclosed
        if (0 == std::strcmp(s.name, name))
            return (uint32_t)std::min<uint64_t>(s.end - s.start, UINT32_MAX);
    }
    return 0;
}

// ── Read side ───────────────────────────────────────────────────────────────

std::vector<FrameStatsChannel::Sample>
FrameStatsChannel::ordered(uint64_t* firstIndex) const {
    const uint64_t first = m_total > kCapacity ? m_total - kCapacity : 0;
    const size_t   n     = (size_t)(m_total - first);
    if (firstIndex) *firstIndex = first;
    std::vector<Sample> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = m_ring[(size_t)((first + i) % kCapacity)];
    return out;
}

void FrameStatsChannel::logSummary(const char* tag, uint32_t warmup) const {
    uint64_t first = 0;
    std::vector<Sample> v = ordered(&first);
    // Warmup only needs skipping while the window still reaches back to frame 0;
    // once the ring has wrapped, those frames are already gone.
    const size_t skip = (first == 0) ? std::min<size_t>(warmup, v.size()) : 0;
    if (v.size() - skip < 2) {
        std::printf("[FrameStats] %s — %zu frames (warming up)\n", tag, v.size());
        return;
    }
    std::vector<double> iv;
    iv.reserve(v.size() - skip);
    for (size_t i = skip; i < v.size(); ++i) iv.push_back(v[i].intervalNs / 1e6);
    const Stats s = statsOf(iv);
    size_t late = 0;
    for (double x : iv) if (x > s.p50 * 1.5) ++late;
    std::printf("[FrameStats] %s — %zu frames | cadence p50 %.2f p99 %.2f "
                "max %.2f ms (%.1f fps) | late %zu (%.2f%%)\n",
                tag, iv.size(), s.p50, s.p99, s.max,
                s.mean > 0.0 ? 1000.0 / s.mean : 0.0,
                late, 100.0 * (double)late / (double)iv.size());
    std::fflush(stdout);
}

void FrameStatsChannel::logDistribution(const char* tag, uint32_t warmup) const {
    uint64_t first = 0;
    std::vector<Sample> v = ordered(&first);
    const size_t skip = (first == 0) ? std::min<size_t>(warmup, v.size()) : 0;
    if (v.size() - skip < 16) {
        std::printf("[FrameStats] %s — only %zu usable frames, not enough for a "
                    "distribution\n", tag, v.size() - skip);
        std::fflush(stdout);
        return;
    }

    std::vector<double> iv, work, pres;
    const size_t n = v.size() - skip;
    iv.reserve(n); work.reserve(n); pres.reserve(n);
    uint64_t presentSeen = 0;
    for (size_t i = skip; i < v.size(); ++i) {
        const Sample& s = v[i];
        iv.push_back(s.intervalNs / 1e6);
        // present is a scope INSIDE the cpu span, so it cannot legitimately
        // exceed it; clamp rather than underflow if a frame ended oddly.
        const uint32_t w = s.cpuNs > s.presentNs ? s.cpuNs - s.presentNs : 0;
        work.push_back(w / 1e6);
        pres.push_back(s.presentNs / 1e6);
        if (s.presentNs) ++presentSeen;
    }

    const Stats sIv   = statsOf(iv);
    const Stats sWork = statsOf(work);
    const Stats sPres = statsOf(pres);

    double totalMs = 0.0;
    for (double x : iv) totalMs += x;

    std::printf("[FrameStats] %s — %zu frames over %.1f s%s\n",
                tag, n, totalMs / 1000.0,
                skip ? " (warmup frames excluded)" : "");
    printRow("cadence", sIv);
    printRow("work",    sWork);
    if (presentSeen)
        printRow("present", sPres);
    else
        std::printf("             present  unavailable — the \"bgfx.frame\" "
                    "scope is compiled out; rebuild with -DENGINE_PROFILE=1 to "
                    "split the vsync wait out of `work`\n");
    std::printf("             mean cadence %.2f ms = %.1f fps\n",
                sIv.mean, sIv.mean > 0.0 ? 1000.0 / sIv.mean : 0.0);
    if (presentSeen && sPres.p50 > 0.0 && sIv.p50 > 0.0)
        std::printf("             present is %.0f%% of the median frame — CPU "
                    "headroom %.2f ms at p99 of work\n",
                    100.0 * sPres.p50 / sIv.p50, sIv.p50 - sWork.p99);

    // ── Dropped-frame table ─────────────────────────────────────────────────
    // For a vsync-locked app the MEDIAN interval IS the display cadence, so
    // interval/median rounds to the number of refresh periods the frame
    // occupied. Everything above 1 is a visible hitch; this, not the mean, is
    // the stutter verdict.
    const double cad = sIv.p50;
    if (cad > 0.0) {
        int mult[5] = {0, 0, 0, 0, 0};   // index 1..4, 4 = "4 or more"
        size_t run = 0, longestRun = 0;
        for (double x : iv) {
            int m = (int)std::lround(x / cad);
            if (m < 1) m = 1;
            if (m > 4) m = 4;
            ++mult[m];
            if (m >= 2) { ++run; longestRun = std::max(longestRun, run); }
            else          run = 0;
        }
        std::printf("\n             vsync multiples (cadence = %.2f ms)\n", cad);
        static const char* kLabel[5] = {
            "", "1x  on time", "2x  1 dropped", "3x  2 dropped", "4x+ 3+ dropped"
        };
        for (int m = 1; m <= 4; ++m) {
            if (!mult[m]) continue;
            std::printf("               %-16s %7d  %6.2f%%\n", kLabel[m],
                        mult[m], 100.0 * mult[m] / (double)n);
        }
        std::printf("               longest consecutive late run: %zu frame%s\n",
                    longestRun, longestRun == 1 ? "" : "s");
    }

    // ── Interval histogram ──────────────────────────────────────────────────
    // Non-empty bins only: a well-paced app then prints two or three rows
    // instead of sixty mostly-zero ones, and a bimodal distribution is obvious
    // at a glance.
    size_t maxBin = 0;
    for (double x : iv) maxBin = std::max(maxBin, (size_t)(x / kBinMs));
    std::vector<size_t> hist(maxBin + 1, 0);
    for (double x : iv) ++hist[(size_t)(x / kBinMs)];
    size_t peak = 0;
    for (size_t c : hist) peak = std::max(peak, c);

    std::printf("\n             interval histogram (%.1f ms bins, non-empty only)\n",
                kBinMs);
    for (size_t b = 0; b < hist.size(); ++b) {
        if (!hist[b]) continue;
        const int cols = peak ? (int)((hist[b] * kBarCols + peak - 1) / peak) : 0;
        std::printf("               %6.1f-%6.1f %7zu  %6.2f%%  ",
                    b * kBinMs, (b + 1) * kBinMs, hist[b],
                    100.0 * (double)hist[b] / (double)n);
        for (int c = 0; c < cols; ++c) std::printf("#");
        std::printf("\n");
    }

    // ── Worst frames ────────────────────────────────────────────────────────
    // Absolute frame indices so a spike can be correlated with what the rest of
    // the log says happened at that frame (a cook finishing, an async upload).
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    const size_t worstN = std::min<size_t>(8, n);
    std::partial_sort(idx.begin(), idx.begin() + (long)worstN, idx.end(),
                      [&](size_t a, size_t b) { return iv[a] > iv[b]; });
    std::printf("\n             worst %zu frames (frame: cadence / work / present ms)\n",
                worstN);
    for (size_t i = 0; i < worstN; ++i) {
        const size_t k = idx[i];
        std::printf("               %8llu: %7.2f / %7.2f / %7.2f\n",
                    (unsigned long long)(first + skip + k),
                    iv[k], work[k], pres[k]);
    }
    std::fflush(stdout);
}

bool FrameStatsChannel::writeCsv(const std::string& path) const {
    uint64_t first = 0;
    const std::vector<Sample> v = ordered(&first);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "frame,interval_ms,work_ms,present_ms\n");
    for (size_t i = 0; i < v.size(); ++i) {
        const Sample& s = v[i];
        const uint32_t w = s.cpuNs > s.presentNs ? s.cpuNs - s.presentNs : 0;
        std::fprintf(f, "%llu,%.4f,%.4f,%.4f\n",
                     (unsigned long long)(first + i),
                     s.intervalNs / 1e6, w / 1e6, s.presentNs / 1e6);
    }
    std::fclose(f);
    return true;
}
