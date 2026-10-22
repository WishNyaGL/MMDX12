#pragma once
#include <filesystem>
#include <memory>
#include <string>

#ifdef ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#endif

namespace mmdx::mocap {

// Process-wide loader: checks whether onnxruntime.dll is available in the executable's directory
// and initializes the ORT C++ API. Missing DLL / unsupported version returns false with a readable
// reason, logged once; never crashes.
bool OrtAvailable(std::string* reason = nullptr);

// Model folder: <exe dir>/mocap/
std::filesystem::path GetMocapModelsDir();
std::filesystem::path GetMocapModelPath(const std::string& filename);

#ifdef ORT_API_MANUAL_INIT
// Process-wide Ort::Env singleton.
Ort::Env& GetOrtEnv();

// Creates an Ort::Session for the specified model with CPU EP, intra-op threads = max(1, hardware_concurrency/2),
// and ORT_ENABLE_ALL graph optimization. If the model file is missing or creation fails, returns nullptr
// and populates *error with a message naming the file.
std::unique_ptr<Ort::Session> CreateOrtSession(const std::filesystem::path& modelPath, std::string* error = nullptr);
#endif

} // namespace mmdx::mocap
