/*
 * main.cpp – OpenCL GPU Compute Microbenchmark
 *
 * Benchmarks two matrix-multiplication kernels:
 *   1. matmul_naive  – pure global-memory accesses
 *   2. matmul_local  – tiled kernel using local (shared) memory
 *
 * For each kernel the program:
 *   • compiles the OpenCL source at runtime
 *   • runs the kernel multiple times and reports average elapsed time
 *   • computes effective memory bandwidth (GB/s)
 *   • validates the result against a CPU reference
 *
 * Build:  see Makefile
 * Usage:  ./matmul_bench [matrix_size]
 *         matrix_size must be a multiple of 16 (default: 1024)
 */

#include <CL/cl.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* Compile-time constants                                               */
/* ------------------------------------------------------------------ */
static constexpr int    TILE_SIZE   = 16;
static constexpr int    WARMUP_RUNS = 2;
static constexpr int    BENCH_RUNS  = 5;
static constexpr float  MAX_REL_ERR = 5e-3f; /* 0.5 % tolerance (fast-relaxed-math) */
static constexpr float  MAX_ABS_ERR = 1e-4f; /* absolute floor for near-zero values */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Stringify an OpenCL error code for readable error messages */
static const char* clErrStr(cl_int err)
{
    switch (err) {
#define CASE(x) case x: return #x
        CASE(CL_SUCCESS);
        CASE(CL_DEVICE_NOT_FOUND);
        CASE(CL_DEVICE_NOT_AVAILABLE);
        CASE(CL_COMPILER_NOT_AVAILABLE);
        CASE(CL_MEM_OBJECT_ALLOCATION_FAILURE);
        CASE(CL_OUT_OF_RESOURCES);
        CASE(CL_OUT_OF_HOST_MEMORY);
        CASE(CL_PROFILING_INFO_NOT_AVAILABLE);
        CASE(CL_MEM_COPY_OVERLAP);
        CASE(CL_IMAGE_FORMAT_MISMATCH);
        CASE(CL_IMAGE_FORMAT_NOT_SUPPORTED);
        CASE(CL_BUILD_PROGRAM_FAILURE);
        CASE(CL_MAP_FAILURE);
        CASE(CL_INVALID_VALUE);
        CASE(CL_INVALID_DEVICE_TYPE);
        CASE(CL_INVALID_PLATFORM);
        CASE(CL_INVALID_DEVICE);
        CASE(CL_INVALID_CONTEXT);
        CASE(CL_INVALID_QUEUE_PROPERTIES);
        CASE(CL_INVALID_COMMAND_QUEUE);
        CASE(CL_INVALID_HOST_PTR);
        CASE(CL_INVALID_MEM_OBJECT);
        CASE(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
        CASE(CL_INVALID_IMAGE_SIZE);
        CASE(CL_INVALID_SAMPLER);
        CASE(CL_INVALID_BINARY);
        CASE(CL_INVALID_BUILD_OPTIONS);
        CASE(CL_INVALID_PROGRAM);
        CASE(CL_INVALID_PROGRAM_EXECUTABLE);
        CASE(CL_INVALID_KERNEL_NAME);
        CASE(CL_INVALID_KERNEL_DEFINITION);
        CASE(CL_INVALID_KERNEL);
        CASE(CL_INVALID_ARG_INDEX);
        CASE(CL_INVALID_ARG_VALUE);
        CASE(CL_INVALID_ARG_SIZE);
        CASE(CL_INVALID_KERNEL_ARGS);
        CASE(CL_INVALID_WORK_DIMENSION);
        CASE(CL_INVALID_WORK_GROUP_SIZE);
        CASE(CL_INVALID_WORK_ITEM_SIZE);
        CASE(CL_INVALID_GLOBAL_OFFSET);
        CASE(CL_INVALID_EVENT_WAIT_LIST);
        CASE(CL_INVALID_EVENT);
        CASE(CL_INVALID_OPERATION);
        CASE(CL_INVALID_GL_OBJECT);
        CASE(CL_INVALID_BUFFER_SIZE);
        CASE(CL_INVALID_MIP_LEVEL);
        CASE(CL_INVALID_GLOBAL_WORK_SIZE);
#undef CASE
        /* ICD loader extension errors */
        case -1001: return "CL_PLATFORM_NOT_FOUND_KHR";
        default: return "UNKNOWN_ERROR";
    }
}

/* Abort on OpenCL error */
#define CL_CHECK(expr) \
    do { \
        cl_int _err = (expr); \
        if (_err != CL_SUCCESS) { \
            fprintf(stderr, "OpenCL error %s (%d) at %s:%d\n", \
                    clErrStr(_err), _err, __FILE__, __LINE__); \
            std::exit(EXIT_FAILURE); \
        } \
    } while (0)

/* Read an entire text file into a std::string */
static std::string readFile(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        std::exit(EXIT_FAILURE);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* ------------------------------------------------------------------ */
/* CPU reference                                                        */
/* ------------------------------------------------------------------ */
static void cpuMatMul(const float* A, const float* B, float* C, int N)
{
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            float s = 0.0f;
            for (int k = 0; k < N; ++k)
                s += A[i * N + k] * B[k * N + j];
            C[i * N + j] = s;
        }
}

/* ------------------------------------------------------------------ */
/* Validation                                                           */
/* ------------------------------------------------------------------ */
static bool validate(const float* ref, const float* result, int N, const char* label)
{
    int    errors   = 0;
    double maxRelErr = 0.0;

    for (int i = 0; i < N * N; ++i) {
        float  diff   = std::fabs(ref[i] - result[i]);
        float  base   = std::max(std::fabs(ref[i]), std::fabs(result[i]));
        /* Pass if the absolute error is tiny OR the relative error is small */
        bool   ok     = (diff <= MAX_ABS_ERR) ||
                        (base > 0.0f && (diff / base) <= MAX_REL_ERR);
        double relErr = (base > 0.0f) ? static_cast<double>(diff / base) : 0.0;
        if (!ok) {
            if (errors < 5)
                fprintf(stderr, "  [%s] mismatch at index %d: ref=%g gpu=%g relErr=%g\n",
                        label, i, static_cast<double>(ref[i]),
                        static_cast<double>(result[i]),
                        static_cast<double>(relErr));
            ++errors;
        }
        if (relErr > maxRelErr)
            maxRelErr = relErr;
    }

    if (errors == 0) {
        printf("  [%s] PASSED  (max relative error = %.2e)\n", label, maxRelErr);
        return true;
    } else {
        printf("  [%s] FAILED  (%d / %d elements mismatched, max rel err = %.2e)\n",
               label, errors, N * N, maxRelErr);
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* OpenCL setup                                                         */
/* ------------------------------------------------------------------ */
struct CLState {
    cl_platform_id   platform;
    cl_device_id     device;
    cl_context       context;
    cl_command_queue queue;   /* profiling-enabled queue */
};

static CLState initCL()
{
    CLState s{};
    cl_uint numPlatforms = 0;
    CL_CHECK(clGetPlatformIDs(0, nullptr, &numPlatforms));
    if (numPlatforms == 0) {
        fprintf(stderr, "No OpenCL platforms found.\n");
        std::exit(EXIT_FAILURE);
    }

    std::vector<cl_platform_id> platforms(numPlatforms);
    CL_CHECK(clGetPlatformIDs(numPlatforms, platforms.data(), nullptr));
    s.platform = platforms[0];

    /* Prefer GPU; fall back to any device */
    cl_int err = clGetDeviceIDs(s.platform, CL_DEVICE_TYPE_GPU, 1, &s.device, nullptr);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "No GPU found, falling back to CL_DEVICE_TYPE_ALL.\n");
        CL_CHECK(clGetDeviceIDs(s.platform, CL_DEVICE_TYPE_ALL, 1, &s.device, nullptr));
    }

    /* Print device name */
    {
        char name[256] = {};
        clGetDeviceInfo(s.device, CL_DEVICE_NAME, sizeof(name), name, nullptr);
        printf("OpenCL device: %s\n", name);
    }

    cl_int ctxErr;
    s.context = clCreateContext(nullptr, 1, &s.device, nullptr, nullptr, &ctxErr);
    CL_CHECK(ctxErr);

    cl_int qErr;
#ifdef CL_VERSION_2_0
    cl_queue_properties qprops[] = {
        CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0
    };
    s.queue = clCreateCommandQueueWithProperties(s.context, s.device, qprops, &qErr);
#else
    s.queue = clCreateCommandQueue(s.context, s.device,
                                   CL_QUEUE_PROFILING_ENABLE, &qErr);
#endif
    CL_CHECK(qErr);

    return s;
}

/* Compile an OpenCL kernel from source and return a cl_kernel handle */
static cl_kernel compileKernel(const CLState& cl,
                                const char*    srcPath,
                                const char*    kernelName)
{
    std::string src = readFile(srcPath);
    const char* srcPtr = src.c_str();
    size_t      srcLen = src.size();

    cl_int      err;
    cl_program  prog = clCreateProgramWithSource(cl.context, 1, &srcPtr, &srcLen, &err);
    CL_CHECK(err);

    err = clBuildProgram(prog, 1, &cl.device, "-cl-fast-relaxed-math", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t logLen = 0;
        clGetProgramBuildInfo(prog, cl.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logLen);
        std::vector<char> log(logLen);
        clGetProgramBuildInfo(prog, cl.device, CL_PROGRAM_BUILD_LOG, logLen, log.data(), nullptr);
        fprintf(stderr, "Build log for %s:\n%s\n", srcPath, log.data());
        std::exit(EXIT_FAILURE);
    }

    cl_kernel k = clCreateKernel(prog, kernelName, &err);
    CL_CHECK(err);
    clReleaseProgram(prog);
    return k;
}

/* ------------------------------------------------------------------ */
/* Benchmark one kernel                                                 */
/* ------------------------------------------------------------------ */
struct BenchResult {
    double avgMs;       /* average kernel execution time in milliseconds  */
    double bandwidthGB; /* effective memory bandwidth in GB/s             */
    bool   correct;
};

static BenchResult runBench(const CLState& cl,
                             cl_kernel      kernel,
                             cl_mem         bufA,
                             cl_mem         bufB,
                             cl_mem         bufC,
                             int            N,
                             const float*   cpuRef,   /* nullptr => skip validation */
                             const char*    label)
{
    const size_t globalSize[2] = { static_cast<size_t>(N), static_cast<size_t>(N) };
    const size_t localSize[2]  = { TILE_SIZE, TILE_SIZE };

    /* Set kernel arguments (same for every run) */
    CL_CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), &bufA));
    CL_CHECK(clSetKernelArg(kernel, 1, sizeof(cl_mem), &bufB));
    CL_CHECK(clSetKernelArg(kernel, 2, sizeof(cl_mem), &bufC));
    CL_CHECK(clSetKernelArg(kernel, 3, sizeof(cl_int),  &N));

    /* Warm-up passes (not timed) */
    for (int r = 0; r < WARMUP_RUNS; ++r) {
        CL_CHECK(clEnqueueNDRangeKernel(cl.queue, kernel, 2, nullptr,
                                        globalSize, localSize,
                                        0, nullptr, nullptr));
    }
    CL_CHECK(clFinish(cl.queue));

    /* Timed passes */
    double totalNs = 0.0;
    cl_event ev;
    for (int r = 0; r < BENCH_RUNS; ++r) {
        CL_CHECK(clEnqueueNDRangeKernel(cl.queue, kernel, 2, nullptr,
                                        globalSize, localSize,
                                        0, nullptr, &ev));
        CL_CHECK(clFinish(cl.queue));

        cl_ulong start, end;
        CL_CHECK(clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START,
                                         sizeof(cl_ulong), &start, nullptr));
        CL_CHECK(clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END,
                                         sizeof(cl_ulong), &end, nullptr));
        totalNs += static_cast<double>(end - start);
        clReleaseEvent(ev);
    }

    double avgMs = (totalNs / BENCH_RUNS) * 1e-6;

    /*
     * Effective memory bandwidth:
     *   Read  2 × N² floats (A and B)
     *   Write 1 × N² floats (C)
     *   Total = 3 × N² × 4 bytes
     */
    double bytes = 3.0 * static_cast<double>(N) * static_cast<double>(N) * sizeof(float);
    double bandwidthGB = (bytes / (avgMs * 1e-3)) / 1e9;

    /* Read back result and validate */
    bool correct = true;
    if (cpuRef != nullptr) {
        std::vector<float> gpuResult(static_cast<size_t>(N) * static_cast<size_t>(N));
        CL_CHECK(clEnqueueReadBuffer(cl.queue, bufC, CL_TRUE, 0,
                                     gpuResult.size() * sizeof(float),
                                     gpuResult.data(), 0, nullptr, nullptr));
        correct = validate(cpuRef, gpuResult.data(), N, label);
    }

    return { avgMs, bandwidthGB, correct };
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char* argv[])
{
    /* Matrix dimension (must be a multiple of TILE_SIZE) */
    int N = 1024;
    if (argc >= 2) {
        N = std::atoi(argv[1]);
        if (N <= 0 || N % TILE_SIZE != 0) {
            fprintf(stderr, "N must be a positive multiple of %d.\n", TILE_SIZE);
            return EXIT_FAILURE;
        }
    }

    printf("=== OpenCL Matrix Multiplication Microbenchmark ===\n");
    printf("Matrix size : %d x %d (%.1f MB per matrix)\n",
           N, N, static_cast<double>(N) * N * sizeof(float) / (1 << 20));
    printf("Tile size   : %d x %d\n", TILE_SIZE, TILE_SIZE);
    printf("Bench runs  : %d (+ %d warm-up)\n\n", BENCH_RUNS, WARMUP_RUNS);

    /* Initialise data on the host */
    const size_t elems = static_cast<size_t>(N) * static_cast<size_t>(N);
    std::vector<float> hA(elems), hB(elems), hCpu(elems);

    std::srand(42);
    for (size_t i = 0; i < elems; ++i) {
        hA[i] = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) - 0.5f;
        hB[i] = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) - 0.5f;
    }

    /* CPU reference (only for small N to keep CI fast) */
    const bool doCpuRef = (N <= 512);
    if (doCpuRef) {
        printf("Computing CPU reference (N=%d)...\n", N);
        cpuMatMul(hA.data(), hB.data(), hCpu.data(), N);
        printf("CPU reference done.\n\n");
    } else {
        printf("N=%d – skipping CPU reference to save time.\n\n", N);
    }

    /* OpenCL setup */
    CLState cl = initCL();

    /* Device buffers */
    cl_int err;
    cl_mem bufA = clCreateBuffer(cl.context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                 elems * sizeof(float), hA.data(), &err);
    CL_CHECK(err);
    cl_mem bufB = clCreateBuffer(cl.context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                 elems * sizeof(float), hB.data(), &err);
    CL_CHECK(err);
    cl_mem bufC = clCreateBuffer(cl.context, CL_MEM_WRITE_ONLY,
                                 elems * sizeof(float), nullptr, &err);
    CL_CHECK(err);

    /* Locate kernel source files relative to the executable                   */
    /* Kernels are expected at ./kernels/ when running from the project root.  */
    const char* naiveSrc = "kernels/matmul_naive.cl";
    const char* localSrc = "kernels/matmul_local.cl";

    /* --- Naive kernel --- */
    printf("--- Naive kernel ---\n");
    cl_kernel naiveKernel = compileKernel(cl, naiveSrc, "matmul_naive");
    BenchResult naiveRes  = runBench(cl, naiveKernel, bufA, bufB, bufC, N,
                                      doCpuRef ? hCpu.data() : nullptr,
                                      "naive");
    printf("  Avg time  : %.3f ms\n",  naiveRes.avgMs);
    printf("  Bandwidth : %.2f GB/s\n\n", naiveRes.bandwidthGB);

    /* --- Local-memory kernel --- */
    printf("--- Local-memory tiled kernel ---\n");
    cl_kernel localKernel = compileKernel(cl, localSrc, "matmul_local");
    BenchResult localRes  = runBench(cl, localKernel, bufA, bufB, bufC, N,
                                      doCpuRef ? hCpu.data() : nullptr,
                                      "local");
    printf("  Avg time  : %.3f ms\n",  localRes.avgMs);
    printf("  Bandwidth : %.2f GB/s\n\n", localRes.bandwidthGB);

    /* --- Summary --- */
    printf("=== Summary ===\n");
    printf("  Naive  : %8.3f ms  %6.2f GB/s\n",
           naiveRes.avgMs, naiveRes.bandwidthGB);
    printf("  Local  : %8.3f ms  %6.2f GB/s\n",
           localRes.avgMs, localRes.bandwidthGB);

    if (naiveRes.avgMs > 0.0) {
        double speedup    = naiveRes.avgMs    / localRes.avgMs;
        double bwImprove  = (localRes.bandwidthGB - naiveRes.bandwidthGB)
                            / naiveRes.bandwidthGB * 100.0;
        printf("  Speedup                   : %.2fx\n", speedup);
        printf("  Bandwidth improvement     : %.1f%%\n", bwImprove);
    }

    bool allCorrect = (!doCpuRef || (naiveRes.correct && localRes.correct));
    printf("  Correctness               : %s\n\n",
           doCpuRef ? (allCorrect ? "PASSED" : "FAILED") : "skipped (N > 512)");

    /* Clean up */
    clReleaseKernel(naiveKernel);
    clReleaseKernel(localKernel);
    clReleaseMemObject(bufA);
    clReleaseMemObject(bufB);
    clReleaseMemObject(bufC);
    clReleaseCommandQueue(cl.queue);
    clReleaseContext(cl.context);

    return allCorrect ? EXIT_SUCCESS : EXIT_FAILURE;
}
