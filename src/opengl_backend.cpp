#ifdef HAVE_OPENGL

#include "opengl_backend.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gpu_bench {

// -----------------------------------------------------------------------
// Shader helpers
// -----------------------------------------------------------------------

std::uint32_t OpenGLBackend::CompileShaderGL(const std::string& path, std::uint32_t type) {
    auto src = ReadFileBytes(path);
    src.push_back('\0');

    GLuint shader = glCreateShader(type);
    const char* srcPtr = src.data();
    glShaderSource(shader, 1, &srcPtr, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error("Shader compilation failed (" + path + "):\n" + log);
    }
    return shader;
}

std::uint32_t OpenGLBackend::LinkProgramGL(std::uint32_t s1, std::uint32_t s2) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, s1);
    if (s2) glAttachShader(prog, s2);
    glLinkProgram(prog);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        glDeleteProgram(prog);
        throw std::runtime_error(std::string("Program link failed:\n") + log);
    }

    glDetachShader(prog, s1);
    glDeleteShader(s1);
    if (s2) { glDetachShader(prog, s2); glDeleteShader(s2); }
    return prog;
}

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------

void OpenGLBackend::InitBackend() {
    std::cout << "[OpenGL Init] Loading GL functions..." << std::endl;
    glfwMakeContextCurrent(window_);
    int gladVer = gladLoadGL(glfwGetProcAddress);
    if (!gladVer)
        throw std::runtime_error("Failed to initialise GLAD");
    std::cout << "[OpenGL Init] GLAD loaded GL "
              << GLAD_VERSION_MAJOR(gladVer) << "."
              << GLAD_VERSION_MINOR(gladVer) << std::endl;

    deviceName_ = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::string glVerStr = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::cout << "[OpenGL Init] " << deviceName_ << "  |  GL " << glVerStr << std::endl;

    if (GLAD_VERSION_MAJOR(gladVer) < 4 ||
        (GLAD_VERSION_MAJOR(gladVer) == 4 && GLAD_VERSION_MINOR(gladVer) < 3)) {
        throw std::runtime_error("OpenGL 4.3+ required for compute shaders");
    }

    if (config_.vsync)
        glfwSwapInterval(1);
    else
        glfwSwapInterval(0);

    std::cout << "[OpenGL Init] Compiling shaders..." << std::endl;
    CreateShaders();
    std::cout << "[OpenGL Init] Creating particle buffers..." << std::endl;
    CreateParticleBuffers();
    std::cout << "[OpenGL Init] Creating timestamp queries..." << std::endl;
    CreateTimestampQueries();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glViewport(0, 0, static_cast<GLsizei>(kWindowWidth),
               static_cast<GLsizei>(kWindowHeight));

    std::cout << "[OpenGL Init] Initialisation complete." << std::endl;
}

void OpenGLBackend::CreateShaders() {
    {
        auto cs = CompileShaderGL(shaderDir_ + "compute_gl.comp", GL_COMPUTE_SHADER);
        computeProgram_ = LinkProgramGL(cs, 0);
    }
    {
        auto vs = CompileShaderGL(shaderDir_ + "particle_gl.vert", GL_VERTEX_SHADER);
        auto fs = CompileShaderGL(shaderDir_ + "particle_gl.frag", GL_FRAGMENT_SHADER);
        renderProgram_ = LinkProgramGL(vs, fs);
    }
}

void OpenGLBackend::CreateParticleBuffers() {
    const GLsizeiptr bufferSize =
        static_cast<GLsizeiptr>(sizeof(Particle) * config_.particleCount);

    glGenBuffers(1, &ssbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_);

    if (config_.hostMemory) {
        GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, bufferSize,
                        initialParticles_.data(), flags);
    } else {
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, bufferSize,
                        initialParticles_.data(), 0);
    }

    std::cout << "Created particle buffers: " << config_.particleCount
              << " particles" << std::endl;

    // VAO — bind the same SSBO as vertex buffer
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, ssbo_);

    // location 0 = position (vec4), location 1 = velocity (vec4)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE,
                          sizeof(Particle), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(Particle),
                          reinterpret_cast<void*>(offsetof(Particle, vx)));
    glBindVertexArray(0);

    // UBO for compute params (deltaTime + bounds, std140 padded to 16 bytes)
    glGenBuffers(1, &ubo_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glBufferData(GL_UNIFORM_BUFFER, sizeof(params), params, GL_DYNAMIC_DRAW);
}

void OpenGLBackend::CreateTimestampQueries() {
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
    timestampsSupported_ = (bits > 0);

    if (!timestampsSupported_) {
        std::cout << "[Profiling] OpenGL timestamp queries not supported." << std::endl;
        return;
    }

    for (int s = 0; s < kTimestampSlotCount; ++s)
        glGenQueries(kTimestampsPerFrame, timestampQueries_[s]);

    std::cout << "[Profiling] OpenGL timestamp queries enabled." << std::endl;
}

// -----------------------------------------------------------------------
// DrawFrame
// -----------------------------------------------------------------------

void OpenGLBackend::DrawFrame(float deltaTime) {
    const int slot = currentFrame_ % kTimestampSlotCount;

    // Collect results from a previous frame (if ring buffer is full)
    if (timestampsSupported_ && timestampFrameCount_ >= kTimestampSlotCount)
        CollectTimestampResults();

    // -- Timestamp: frame begin --
    if (timestampsSupported_)
        glQueryCounter(timestampQueries_[slot][0], GL_TIMESTAMP);

    // -- Compute dispatch --
    float params[4] = {deltaTime, 1.0f, 0.0f, 0.0f};
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(params), params);

    glUseProgram(computeProgram_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo_);

    GLuint groups = (config_.particleCount + kComputeWorkGroupSize - 1)
                    / kComputeWorkGroupSize;
    glDispatchCompute(groups, 1, 1);

    // -- Timestamp: compute end --
    if (timestampsSupported_)
        glQueryCounter(timestampQueries_[slot][1], GL_TIMESTAMP);

    // Barrier: ensure compute writes are visible to vertex fetch
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

    // -- Render --
    glClearColor(0.0f, 0.0f, 0.02f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // -- Timestamp: render begin --
    if (timestampsSupported_)
        glQueryCounter(timestampQueries_[slot][2], GL_TIMESTAMP);

    glUseProgram(renderProgram_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(config_.particleCount));

    // -- Timestamp: render end --
    if (timestampsSupported_)
        glQueryCounter(timestampQueries_[slot][3], GL_TIMESTAMP);

    glfwSwapBuffers(window_);

    currentFrame_++;
    if (timestampsSupported_ && timestampFrameCount_ < kTimestampSlotCount)
        ++timestampFrameCount_;
}

void OpenGLBackend::CollectTimestampResults() {
    const int readSlot = (currentFrame_) % kTimestampSlotCount;

    GLint available = GL_FALSE;
    glGetQueryObjectiv(timestampQueries_[readSlot][3],
                       GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return;

    GLuint64 ts[kTimestampsPerFrame]{};
    for (int i = 0; i < kTimestampsPerFrame; ++i)
        glGetQueryObjectui64v(timestampQueries_[readSlot][i],
                              GL_QUERY_RESULT, &ts[i]);

    double computeMs = static_cast<double>(ts[1] - ts[0]) / 1e6;
    double renderMs  = static_cast<double>(ts[3] - ts[2]) / 1e6;
    double totalMs   = static_cast<double>(ts[3] - ts[0]) / 1e6;

    if (computeMs >= 0.0 && renderMs >= 0.0 && totalMs >= 0.0)
        AccumulateTiming(computeMs, renderMs, totalMs);
}

// -----------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------

void OpenGLBackend::CleanupBackend() {
    if (timestampsSupported_) {
        for (int s = 0; s < kTimestampSlotCount; ++s)
            glDeleteQueries(kTimestampsPerFrame, timestampQueries_[s]);
    }

    if (ubo_)            { glDeleteBuffers(1, &ubo_);            ubo_ = 0; }
    if (vao_)            { glDeleteVertexArrays(1, &vao_);       vao_ = 0; }
    if (ssbo_)           { glDeleteBuffers(1, &ssbo_);           ssbo_ = 0; }
    if (computeProgram_) { glDeleteProgram(computeProgram_);     computeProgram_ = 0; }
    if (renderProgram_)  { glDeleteProgram(renderProgram_);      renderProgram_ = 0; }
}

void OpenGLBackend::WaitIdle() {
    glFinish();
}

}  // namespace gpu_bench

#endif  // HAVE_OPENGL
