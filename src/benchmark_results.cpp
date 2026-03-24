#include "benchmark_results.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <cstdlib>
#endif

namespace gpu_bench {

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static std::string ResultsDir() {
#ifdef _WIN32
    _mkdir("results");
#else
    mkdir("results", 0755);
#endif
    return "results";
}

std::string ResultsFilePath() {
    return ResultsDir() + "/results.json";
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;
        }
    }
    return out;
}

static std::string JsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  out += '"';  ++i; break;
            case '\\': out += '\\'; ++i; break;
            case 'n':  out += '\n'; ++i; break;
            case 'r':  out += '\r'; ++i; break;
            case 't':  out += '\t'; ++i; break;
            default:   out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ID / timestamp generation
// ---------------------------------------------------------------------------

std::string GenerateResultId() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d-%H%M%S")
        << "-" << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string GenerateTimestamp() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

static std::string ResultToJson(const BenchmarkResult& r, int indent = 4) {
    std::string pad(indent, ' ');
    std::ostringstream o;
    o << std::fixed;

    o << pad << "{\n";
    auto str = [&](const char* k, const std::string& v) {
        o << pad << "  \"" << k << "\": \"" << JsonEscape(v) << "\",\n";
    };
    auto u32 = [&](const char* k, std::uint32_t v) {
        o << pad << "  \"" << k << "\": " << v << ",\n";
    };
    auto bl = [&](const char* k, bool v) {
        o << pad << "  \"" << k << "\": " << (v ? "true" : "false") << ",\n";
    };
    auto dbl = [&](const char* k, double v, int prec = 3) {
        o << pad << "  \"" << k << "\": " << std::setprecision(prec) << v << ",\n";
    };

    str("id",          r.id);
    str("timestamp",   r.timestamp);
    str("graphicsApi",    r.graphicsApi);
    str("deviceName",     r.deviceName);
    str("driverVersion",  r.driverVersion);
    str("cpuName",        r.cpuName);
    str("osVersion",      r.osVersion);
    str("memory",         r.memory);
    u32("resWidth",    r.resWidth);
    u32("resHeight",   r.resHeight);
    u32("particleCount", r.particleCount);
    str("difficulty",  r.difficulty);
    bl ("vsync",       r.vsync);
    bl ("isSoftware",  r.isSoftware);
    bl ("headless",    r.headless);
    u32("framesInFlight", r.framesInFlight);

    dbl("durationSec",    r.durationSec, 1);
    dbl("warmupSec",      r.warmupSec, 1);
    u32("measuredFrames", r.measuredFrames);
    u32("timingSamples",  r.timingSamples);

    dbl("avgComputeMs",  r.avgComputeMs);
    dbl("minComputeMs",  r.minComputeMs);
    dbl("maxComputeMs",  r.maxComputeMs);
    dbl("avgRenderMs",   r.avgRenderMs);
    dbl("minRenderMs",   r.minRenderMs);
    dbl("maxRenderMs",   r.maxRenderMs);
    dbl("avgTotalGpuMs", r.avgTotalGpuMs);
    dbl("minTotalGpuMs", r.minTotalGpuMs);
    dbl("maxTotalGpuMs", r.maxTotalGpuMs);

    dbl("avgFps",         r.avgFps, 1);
    dbl("avgFrameTimeMs", r.avgFrameTimeMs);
    dbl("gpuUtilisation", r.gpuUtilisation, 1);

    o << pad << "  \"bottleneck\": \"" << JsonEscape(r.bottleneck) << "\"\n";
    o << pad << "}";
    return o.str();
}

// ---------------------------------------------------------------------------
// Deserialize
// ---------------------------------------------------------------------------

static BenchmarkResult JsonToResult(const std::string& json) {
    BenchmarkResult r;

    auto findStr = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        auto q1 = json.find('"', pos + 1);
        if (q1 == std::string::npos) return "";
        auto q2 = q1 + 1;
        while (q2 < json.size() && !(json[q2] == '"' && json[q2 - 1] != '\\'))
            ++q2;
        return JsonUnescape(json.substr(q1 + 1, q2 - q1 - 1));
    };

    auto findNum = [&](const char* key) -> double {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return 0.0;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return 0.0;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
            ++pos;
        return std::stod(json.substr(pos));
    };

    auto findBool = [&](const char* key) -> bool {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return false;
        return json.find("true", pos) < json.find('\n', pos);
    };

    r.id            = findStr("id");
    r.timestamp     = findStr("timestamp");
    r.graphicsApi    = findStr("graphicsApi");
    r.deviceName     = findStr("deviceName");
    r.driverVersion  = findStr("driverVersion");
    r.cpuName        = findStr("cpuName");
    r.osVersion      = findStr("osVersion");
    r.memory         = findStr("memory");
    r.resWidth      = static_cast<std::uint32_t>(findNum("resWidth"));
    r.resHeight     = static_cast<std::uint32_t>(findNum("resHeight"));
    r.particleCount = static_cast<std::uint32_t>(findNum("particleCount"));
    r.difficulty    = findStr("difficulty");
    r.vsync         = findBool("vsync");
    r.isSoftware    = findBool("isSoftware");
    r.headless      = findBool("headless");
    r.framesInFlight = static_cast<std::uint32_t>(findNum("framesInFlight"));
    if (r.framesInFlight == 0) r.framesInFlight = 2;  // default for old results

    r.durationSec    = findNum("durationSec");
    r.warmupSec      = findNum("warmupSec");
    r.measuredFrames = static_cast<std::uint32_t>(findNum("measuredFrames"));
    r.timingSamples  = static_cast<std::uint32_t>(findNum("timingSamples"));

    r.avgComputeMs  = findNum("avgComputeMs");
    r.minComputeMs  = findNum("minComputeMs");
    r.maxComputeMs  = findNum("maxComputeMs");
    r.avgRenderMs   = findNum("avgRenderMs");
    r.minRenderMs   = findNum("minRenderMs");
    r.maxRenderMs   = findNum("maxRenderMs");
    r.avgTotalGpuMs = findNum("avgTotalGpuMs");
    r.minTotalGpuMs = findNum("minTotalGpuMs");
    r.maxTotalGpuMs = findNum("maxTotalGpuMs");

    r.avgFps         = findNum("avgFps");
    r.avgFrameTimeMs = findNum("avgFrameTimeMs");
    r.gpuUtilisation = findNum("gpuUtilisation");
    r.bottleneck     = findStr("bottleneck");

    return r;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

std::vector<BenchmarkResult> LoadResults() {
    std::vector<BenchmarkResult> results;
    std::ifstream in(ResultsFilePath());
    if (!in.is_open()) return results;

    std::string all((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();

    size_t pos = 0;
    while (pos < all.size()) {
        auto start = all.find('{', pos);
        if (start == std::string::npos) break;
        int depth = 1;
        auto end = start + 1;
        while (end < all.size() && depth > 0) {
            if (all[end] == '{') ++depth;
            if (all[end] == '}') --depth;
            ++end;
        }
        if (depth == 0) {
            results.push_back(JsonToResult(all.substr(start, end - start)));
        }
        pos = end;
    }
    return results;
}

bool SaveResults(const std::vector<BenchmarkResult>& results) {
    ResultsDir();
    std::ofstream out(ResultsFilePath());
    if (!out.is_open()) return false;

    out << "[\n";
    for (size_t i = 0; i < results.size(); ++i) {
        out << ResultToJson(results[i]);
        if (i + 1 < results.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
    out.close();
    return true;
}

bool AppendResult(const BenchmarkResult& r) {
    auto results = LoadResults();
    results.push_back(r);
    return SaveResults(results);
}

bool DeleteResult(const std::string& id) {
    auto results = LoadResults();
    auto it = std::remove_if(results.begin(), results.end(),
        [&](const BenchmarkResult& r) { return r.id == id; });
    if (it == results.end()) return false;
    results.erase(it, results.end());
    return SaveResults(results);
}

bool ClearResults() {
    return SaveResults({});
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void PrintResultsTable(const std::vector<BenchmarkResult>& results) {
    if (results.empty()) {
        std::cout << "No benchmark results saved.\n"
                  << "Results file: " << ResultsFilePath() << "\n";
        return;
    }

    std::cout << "\n"
        "==========================================================\n"
        "                   Saved Benchmark Results\n"
        "==========================================================\n\n";

    std::cout << std::left
              << std::setw(4)  << "#"
              << std::setw(8)  << "API"
              << std::setw(28) << "Device"
              << std::setw(10) << "Difficulty"
              << std::setw(10) << "Avg FPS"
              << std::setw(12) << "GPU Avg(ms)"
              << "\n";
    std::cout << std::string(72, '-') << "\n";

    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::string dev = r.deviceName;
        if (dev.size() > 26) dev = dev.substr(0, 23) + "...";

        std::cout << std::left
                  << std::setw(4)  << (i + 1)
                  << std::setw(8)  << r.graphicsApi
                  << std::setw(28) << dev
                  << std::setw(10) << r.difficulty
                  << std::setw(10) << static_cast<int>(r.avgFps);

        if (r.timingSamples > 0)
            std::cout << std::fixed << std::setprecision(3)
                      << std::setw(12) << r.avgTotalGpuMs;
        else
            std::cout << std::setw(12) << "N/A";

        std::cout << "\n";
    }

    std::cout << "\nTotal: " << results.size() << " result(s)\n"
              << "File:  " << ResultsFilePath() << "\n"
              << "==========================================================\n\n";
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

static std::string DeltaStr(double baseline, double value, bool higherIsBetter) {
    if (baseline == 0.0) return "--";
    double pct = (value - baseline) / baseline * 100.0;
    std::ostringstream o;
    o << std::fixed << std::setprecision(1);
    if (pct > 0.0) o << (higherIsBetter ? "+" : "+") << pct << "%";
    else if (pct < 0.0) o << pct << "%";
    else o << "0.0%";
    return o.str();
}

void PrintComparisonTable(const std::vector<BenchmarkResult>& results) {
    if (results.empty()) {
        std::cout << "No benchmark results to compare.\n";
        return;
    }
    if (results.size() < 2) {
        std::cout << "Need at least 2 results to compare. Currently saved: "
                  << results.size() << "\n";
        return;
    }

    auto sorted = results;
    std::sort(sorted.begin(), sorted.end(),
        [](const BenchmarkResult& a, const BenchmarkResult& b) {
            return a.avgFps > b.avgFps;
        });

    const double baselineFps = sorted[0].avgFps;

    std::cout << "\n"
        "==========================================================\n"
        "                  Benchmark Comparison\n"
        "==========================================================\n\n";

    std::cout << std::left
              << std::setw(4)  << "#"
              << std::setw(8)  << "API"
              << std::setw(26) << "GPU"
              << std::setw(10) << "Diff."
              << std::right
              << std::setw(9)  << "Avg FPS"
              << std::setw(12) << "GPU ms"
              << std::setw(10) << "Delta"
              << "\n";
    std::cout << std::string(79, '-') << "\n";

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& r = sorted[i];
        std::string dev = r.deviceName;
        if (dev.size() > 24) dev = dev.substr(0, 21) + "...";

        std::cout << std::left
                  << std::setw(4)  << (i + 1)
                  << std::setw(8)  << r.graphicsApi
                  << std::setw(26) << dev
                  << std::setw(10) << r.difficulty
                  << std::right
                  << std::setw(9)  << static_cast<int>(r.avgFps);

        if (r.timingSamples > 0)
            std::cout << std::fixed << std::setprecision(3)
                      << std::setw(12) << r.avgTotalGpuMs;
        else
            std::cout << std::setw(12) << "N/A";

        if (i == 0)
            std::cout << std::setw(10) << "baseline";
        else
            std::cout << std::setw(10) << DeltaStr(baselineFps, r.avgFps, true);

        std::cout << "\n";
    }

    std::cout << "\nTotal: " << sorted.size() << " result(s), ranked by Avg FPS (descending)\n"
              << "==========================================================\n\n";
}

void PrintDetailedComparison(const BenchmarkResult& a, const BenchmarkResult& b) {
    const int w1 = 16, w2 = 24, w3 = 24, w4 = 10;

    std::cout << "\n"
        "==========================================================\n"
        "              Benchmark Comparison (A vs B)\n"
        "==========================================================\n";

    std::cout << std::left
              << std::setw(w1) << ""
              << std::setw(w2) << "A"
              << std::setw(w3) << "B"
              << std::setw(w4) << "Delta"
              << "\n";
    std::cout << std::string(w1 + w2 + w3 + w4, '-') << "\n";

    auto rowStr = [&](const char* label, const std::string& va, const std::string& vb) {
        std::cout << std::left
                  << std::setw(w1) << label
                  << std::setw(w2) << va
                  << std::setw(w3) << vb
                  << "\n";
    };

    auto rowFps = [&](const char* label, double va, double vb) {
        std::cout << std::left << std::setw(w1) << label
                  << std::setw(w2) << static_cast<int>(va)
                  << std::setw(w3) << static_cast<int>(vb)
                  << std::setw(w4) << DeltaStr(va, vb, true)
                  << "\n";
    };

    auto rowMs = [&](const char* label, double va, double vb) {
        std::ostringstream oa, ob;
        oa << std::fixed << std::setprecision(3) << va << " ms";
        ob << std::fixed << std::setprecision(3) << vb << " ms";
        std::cout << std::left << std::setw(w1) << label
                  << std::setw(w2) << oa.str()
                  << std::setw(w3) << ob.str()
                  << std::setw(w4) << DeltaStr(va, vb, false)
                  << "\n";
    };

    auto rowPct = [&](const char* label, double va, double vb) {
        std::ostringstream oa, ob;
        oa << std::fixed << std::setprecision(1) << (va * 100.0) << "%";
        ob << std::fixed << std::setprecision(1) << (vb * 100.0) << "%";
        std::cout << std::left << std::setw(w1) << label
                  << std::setw(w2) << oa.str()
                  << std::setw(w3) << ob.str()
                  << "\n";
    };

    rowStr("ID:",          a.id,          b.id);
    rowStr("Timestamp:",   a.timestamp,   b.timestamp);
    std::cout << "\n";

    rowStr("Graphics API:", a.graphicsApi, b.graphicsApi);
    rowStr("GPU:",          a.deviceName,  b.deviceName);
    rowStr("Driver:",       a.driverVersion, b.driverVersion);
    rowStr("CPU:",          a.cpuName,     b.cpuName);
    rowStr("OS:",           a.osVersion,   b.osVersion);
    rowStr("Memory:",       a.memory,      b.memory);

    std::string resA = std::to_string(a.resWidth) + "x" + std::to_string(a.resHeight);
    std::string resB = std::to_string(b.resWidth) + "x" + std::to_string(b.resHeight);
    rowStr("Resolution:",   resA, resB);

    std::string partA = std::to_string(a.particleCount) + " (" + a.difficulty + ")";
    std::string partB = std::to_string(b.particleCount) + " (" + b.difficulty + ")";
    rowStr("Particles:",    partA, partB);

    rowStr("V-Sync:",       a.vsync ? "ON" : "OFF", b.vsync ? "ON" : "OFF");
    std::cout << "\n";

    rowFps("Avg FPS:",      a.avgFps, b.avgFps);
    rowMs ("Frame Time:",   a.avgFrameTimeMs, b.avgFrameTimeMs);
    std::cout << "\n";

    if (a.timingSamples > 0 || b.timingSamples > 0) {
        rowMs("Avg Compute:",  a.avgComputeMs,  b.avgComputeMs);
        rowMs("Avg Render:",   a.avgRenderMs,   b.avgRenderMs);
        rowMs("Avg GPU Total:",a.avgTotalGpuMs, b.avgTotalGpuMs);
        rowPct("GPU Util:",    a.gpuUtilisation, b.gpuUtilisation);
        std::cout << "\n";
    }

    rowStr("Bottleneck:",   a.bottleneck, b.bottleneck);

    std::cout << "==========================================================\n\n";
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

bool ExportResultsCsv(const std::string& path,
                      const std::vector<BenchmarkResult>& results) {
    std::ofstream out(path);
    if (!out.is_open()) return false;

    out << "id,timestamp,graphicsApi,deviceName,driverVersion,cpuName,osVersion,memory,"
           "resolution,particleCount,difficulty,vsync,isSoftware,"
           "durationSec,warmupSec,measuredFrames,timingSamples,"
           "avgComputeMs,minComputeMs,maxComputeMs,"
           "avgRenderMs,minRenderMs,maxRenderMs,"
           "avgTotalGpuMs,minTotalGpuMs,maxTotalGpuMs,"
           "avgFps,avgFrameTimeMs,gpuUtilisation,bottleneck\n";

    for (const auto& r : results) {
        auto q = [](const std::string& s) {
            return "\"" + s + "\"";
        };
        out << std::fixed << std::setprecision(3)
            << q(r.id) << ","
            << q(r.timestamp) << ","
            << q(r.graphicsApi) << ","
            << q(r.deviceName) << ","
            << q(r.driverVersion) << ","
            << q(r.cpuName) << ","
            << q(r.osVersion) << ","
            << q(r.memory) << ","
            << r.resWidth << "x" << r.resHeight << ","
            << r.particleCount << ","
            << q(r.difficulty) << ","
            << (r.vsync ? "true" : "false") << ","
            << (r.isSoftware ? "true" : "false") << ","
            << std::setprecision(1) << r.durationSec << ","
            << r.warmupSec << ","
            << r.measuredFrames << ","
            << r.timingSamples << ","
            << std::setprecision(3)
            << r.avgComputeMs << ","
            << r.minComputeMs << ","
            << r.maxComputeMs << ","
            << r.avgRenderMs << ","
            << r.minRenderMs << ","
            << r.maxRenderMs << ","
            << r.avgTotalGpuMs << ","
            << r.minTotalGpuMs << ","
            << r.maxTotalGpuMs << ","
            << std::setprecision(1) << r.avgFps << ","
            << std::setprecision(3) << r.avgFrameTimeMs << ","
            << std::setprecision(1) << r.gpuUtilisation << ","
            << q(r.bottleneck) << "\n";
    }

    out.close();
    return true;
}

}  // namespace gpu_bench
