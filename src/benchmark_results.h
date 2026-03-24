#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu_bench {

struct BenchmarkResult {
    std::string id;
    std::string timestamp;

    std::string graphicsApi;
    std::string deviceName;
    std::string driverVersion;
    std::string cpuName;
    std::string osVersion;
    std::string memory;
    std::uint32_t resWidth  = 0;
    std::uint32_t resHeight = 0;
    std::uint32_t particleCount = 0;
    std::string difficulty;
    bool vsync      = false;
    bool isSoftware = false;
    bool headless   = false;
    std::uint32_t framesInFlight = 2;

    double durationSec   = 0.0;
    double warmupSec     = 0.0;
    std::uint32_t measuredFrames  = 0;
    std::uint32_t timingSamples   = 0;

    double avgComputeMs  = 0.0, minComputeMs  = 0.0, maxComputeMs  = 0.0;
    double avgRenderMs   = 0.0, minRenderMs   = 0.0, maxRenderMs   = 0.0;
    double avgTotalGpuMs = 0.0, minTotalGpuMs = 0.0, maxTotalGpuMs = 0.0;

    double avgFps          = 0.0;
    double avgFrameTimeMs  = 0.0;
    double gpuUtilisation  = 0.0;
    std::string bottleneck;
};

std::string ResultsFilePath();

std::string GenerateResultId();
std::string GenerateTimestamp();

std::vector<BenchmarkResult> LoadResults();
bool SaveResults(const std::vector<BenchmarkResult>& results);
bool AppendResult(const BenchmarkResult& r);
bool DeleteResult(const std::string& id);
bool ClearResults();

void PrintResultsTable(const std::vector<BenchmarkResult>& results);
void PrintComparisonTable(const std::vector<BenchmarkResult>& results);
void PrintDetailedComparison(const BenchmarkResult& a, const BenchmarkResult& b);
bool ExportResultsCsv(const std::string& path,
                      const std::vector<BenchmarkResult>& results);

}  // namespace gpu_bench
