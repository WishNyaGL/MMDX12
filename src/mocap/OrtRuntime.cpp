#include "OrtRuntime.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <thread>

namespace mmdx::mocap {

namespace {

std::filesystem::path QueryExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buf).parent_path();
}

struct OrtState {
    std::mutex mutex;
    bool attempted = false;
    bool available = false;
    std::string reason;
    HMODULE hDll = nullptr;
    std::unique_ptr<Ort::Env> env;
};

OrtState& GetState() {
    static OrtState state;
    return state;
}

} // namespace

std::filesystem::path GetMocapModelsDir() {
    return QueryExeDir() / L"mocap";
}

std::filesystem::path GetMocapModelPath(const std::string& filename) {
    return GetMocapModelsDir() / filename;
}

bool OrtAvailable(std::string* reason) {
    OrtState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.attempted) {
        state.attempted = true;
        const std::filesystem::path exeDir = QueryExeDir();
        const std::filesystem::path dllPath = exeDir / L"onnxruntime.dll";

        state.hDll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!state.hDll) {
            const DWORD err = GetLastError();
            state.reason = "onnxruntime.dll not found in " + PathToUtf8(dllPath) + " (error " + std::to_string(err) + ")";
            LOG_WARN("%s", state.reason.c_str());
            if (reason) *reason = state.reason;
            return false;
        }

        using OrtGetApiBaseFn = const OrtApiBase* (ORT_API_CALL*)();
        auto pfnGetApiBase = reinterpret_cast<OrtGetApiBaseFn>(GetProcAddress(state.hDll, "OrtGetApiBase"));
        if (!pfnGetApiBase) {
            state.reason = "OrtGetApiBase symbol not found in " + PathToUtf8(dllPath);
            LOG_WARN("%s", state.reason.c_str());
            if (reason) *reason = state.reason;
            return false;
        }

        const OrtApiBase* apiBase = pfnGetApiBase();
        if (!apiBase) {
            state.reason = "OrtGetApiBase() returned null from " + PathToUtf8(dllPath);
            LOG_WARN("%s", state.reason.c_str());
            if (reason) *reason = state.reason;
            return false;
        }

        const OrtApi* api = apiBase->GetApi(ORT_API_VERSION);
        if (!api) {
            state.reason = "onnxruntime.dll does not support ORT_API_VERSION " + std::to_string(ORT_API_VERSION);
            LOG_WARN("%s", state.reason.c_str());
            if (reason) *reason = state.reason;
            return false;
        }

        Ort::InitApi(api);
        try {
            state.env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "mmdx_mocap");
            state.available = true;
            LOG_INFO("ORT loaded: onnxruntime.dll version %s", apiBase->GetVersionString());
        } catch (const Ort::Exception& e) {
            state.reason = std::string("Failed to initialize Ort::Env: ") + e.what();
            LOG_WARN("%s", state.reason.c_str());
            if (reason) *reason = state.reason;
            return false;
        }
    }

    if (reason && !state.reason.empty()) {
        *reason = state.reason;
    }
    return state.available;
}

#ifdef ORT_API_MANUAL_INIT
Ort::Env& GetOrtEnv() {
    OrtState& state = GetState();
    return *state.env;
}

std::unique_ptr<Ort::Session> CreateOrtSession(const std::filesystem::path& modelPath, std::string* error) {
    std::string reason;
    if (!OrtAvailable(&reason)) {
        if (error) *error = reason;
        return nullptr;
    }

    if (!std::filesystem::exists(modelPath)) {
        std::string err = "mocap model file missing: " + PathToUtf8(modelPath);
        LOG_WARN("%s", err.c_str());
        if (error) *error = std::move(err);
        return nullptr;
    }

    try {
        Ort::SessionOptions sessionOptions;
        const unsigned int threads = std::max(1u, std::thread::hardware_concurrency() / 2);
        sessionOptions.SetIntraOpNumThreads(threads);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        return std::make_unique<Ort::Session>(GetOrtEnv(), modelPath.c_str(), sessionOptions);
    } catch (const Ort::Exception& e) {
        std::string err = "failed to create Ort::Session for " + PathToUtf8(modelPath) + ": " + e.what();
        LOG_WARN("%s", err.c_str());
        if (error) *error = std::move(err);
        return nullptr;
    }
}
#endif

} // namespace mmdx::mocap
