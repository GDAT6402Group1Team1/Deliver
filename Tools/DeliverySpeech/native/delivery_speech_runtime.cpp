#include "delivery_speech_runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct ModelFile {
    const char* name;
    uint64_t size;
    const char* sha256;
};

// 只加载已经固定并检查过的模型，避免上游读取损坏配置后直接终止进程
constexpr ModelFile ModelFiles[] = {
    {"encoder.int8.onnx", 26246290, "da5009f42dd5af5a26cca798cd9365aafcd5fd85e377df1fcc8a75624ebc2108"},
    {"decoder.onnx", 12408860, "53aee85802b4d57237457382aa92ceb18811b3333650ef008e3864be59abf7ff"},
    {"joiner.int8.onnx", 2858478, "478a56c0d7bc25eaee4f70af5909f836a910da1bae19d41c669a0863e5fee05a"},
    {"tokens.txt", 48697, "8b294db9045d6e5f94647f4c1eec1af4da143a75053c399611444b378ff966ac"}
};

void CheckNt(NTSTATUS status) {
    if (status < 0) throw std::runtime_error("SHA256 provider failed");
}

struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Sha256() {
        CheckNt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        const NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
        if (status < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            algorithm = nullptr;
            CheckNt(status);
        }
    }
    ~Sha256() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

std::string FileHash(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open model file");
    Sha256 state;
    std::array<char, 65536> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0) CheckNt(BCryptHashData(state.hash,
            reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0));
    }
    if (!input.eof()) throw std::runtime_error("Unable to read model file");
    std::array<unsigned char, 32> digest;
    CheckNt(BCryptFinishHash(state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    std::ostringstream result;
    for (const auto byte : digest) result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return result.str();
}

std::string ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Unable to read tokens file");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool ValidUtf8(const char* value, size_t size) {
    return size == 0 || (size < INT_MAX && MultiByteToWideChar(CP_UTF8,
        MB_ERR_INVALID_CHARS, value, static_cast<int>(size), nullptr, 0) > 0);
}

std::string BoundedUtf8(const char* value, size_t limit, const char* name) {
    if (!value) return {};
    const size_t size = strnlen_s(value, limit + 1);
    if (size > limit || !ValidUtf8(value, size)) throw std::invalid_argument(name);
    return std::string(value, size);
}

// 热词只能包含模型现有的字，避免无效 token 在上游构图时触发断言
void ValidateHotwords(const std::string& hotwords, const std::string& tokens) {
    std::unordered_set<std::string> vocabulary;
    std::istringstream token_stream(tokens);
    std::string line;
    while (std::getline(token_stream, line)) {
        const auto separator = line.find_last_of(" \t");
        if (separator != std::string::npos) vocabulary.insert(line.substr(0, separator));
    }
    for (size_t offset = 0; offset < hotwords.size();) {
        const auto first = static_cast<unsigned char>(hotwords[offset]);
        const size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        const auto character = hotwords.substr(offset, width);
        offset += width;
        if (character == " " || character == "\t" || character == "\n" || character == "\r") continue;
        if (character == ":" || character == "/" || vocabulary.count(character) == 0)
            throw std::invalid_argument("Hotwords must contain only characters from the pinned Chinese vocabulary");
    }
}

int32_t CopyText(const std::string& value, char* output, uint32_t capacity, uint32_t* required) {
    const auto needed = static_cast<uint32_t>(value.size() + 1);
    if (required) *required = needed;
    if (!output || capacity < needed) {
        if (output && capacity) output[0] = '\0';
        return DSR_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, value.c_str(), needed);
    return DSR_OK;
}

void CopyError(const char* value, char* output, uint32_t capacity) noexcept {
    if (!output || capacity == 0) return;
    const size_t size = std::min<size_t>(std::strlen(value), capacity - 1);
    std::memcpy(output, value, size);
    output[size] = '\0';
}

bool SupportedRate(int32_t rate) {
    return rate == 8000 || rate == 16000 || rate == 22050 || rate == 24000 ||
           rate == 32000 || rate == 44100 || rate == 48000 || rate == 96000 || rate == 192000;
}
}

struct DsrHandle {
    const SherpaOnnxOnlineRecognizer* recognizer = nullptr;
    const SherpaOnnxOnlineStream* stream = nullptr;
    uint64_t turn = 0;
    uint64_t revision = 0;
    uint64_t samples = 0;
    int32_t sample_rate = 0;
    uint32_t max_utterance_ms = 15000;
    uint32_t state = DSR_IDLE;
    bool has_nonzero_input = false;
    std::string text;
    std::string error;

    ~DsrHandle() {
        SherpaOnnxDestroyOnlineStream(stream);
        SherpaOnnxDestroyOnlineRecognizer(recognizer);
    }
    void ReleaseStream() {
        SherpaOnnxDestroyOnlineStream(stream);
        stream = nullptr;
    }
    int32_t Fail(int32_t status, const char* reason) {
        error = reason;
        return status;
    }
    void Refresh() {
        while (SherpaOnnxIsOnlineStreamReady(recognizer, stream))
            SherpaOnnxDecodeOnlineStream(recognizer, stream);
        const auto* result = SherpaOnnxGetOnlineStreamResult(recognizer, stream);
        if (!result) throw std::runtime_error("Recognizer did not return a result");
        const std::unique_ptr<const SherpaOnnxOnlineRecognizerResult,
            decltype(&SherpaOnnxDestroyOnlineRecognizerResult)> owner(result, SherpaOnnxDestroyOnlineRecognizerResult);
        const std::string updated = result->text ? result->text : "";
        if (updated != text) { text = updated; ++revision; }
    }
};

namespace {
// 上游异常留在 DLL 内，失败后取消 stream，防止继续使用部分更新的识别状态
template <typename Operation>
int32_t Guard(DsrHandle* handle, Operation operation) noexcept {
    if (!handle) return DSR_INVALID_ARGUMENT;
    try { return operation(); }
    catch (const std::exception& error) {
        try {
            handle->ReleaseStream();
            handle->state = DSR_CANCELLED;
            handle->text.clear();
            ++handle->revision;
            handle->error = error.what();
        } catch (...) {}
    } catch (...) {
        try {
            handle->ReleaseStream();
            handle->state = DSR_CANCELLED;
            handle->text.clear();
            ++handle->revision;
            handle->error = "Unknown native recognition error";
        } catch (...) {}
    }
    return DSR_RUNTIME_ERROR;
}
}

uint32_t dsr_abi_version(void) { return DSR_ABI_VERSION; }

int32_t dsr_create(const DsrConfig* config, DsrHandle** out_handle,
                   char* error_utf8, uint32_t error_capacity) {
    if (out_handle) *out_handle = nullptr;
    if (error_utf8 && error_capacity) error_utf8[0] = '\0';
    try {
        if (!config || !out_handle || config->struct_size != sizeof(DsrConfig) ||
            config->abi_version != DSR_ABI_VERSION || config->reserved != 0 ||
            config->num_threads > 2 || config->max_utterance_ms > 30000 ||
            (config->max_utterance_ms != 0 && config->max_utterance_ms < 100) ||
            !std::isfinite(config->hotword_score) || config->hotword_score < 0 || config->hotword_score > 5)
            throw std::invalid_argument("Invalid speech runtime ABI or configuration");
        const auto model_directory = BoundedUtf8(config->model_directory_utf8, 32760, "Invalid UTF-8 model directory");
        if (model_directory.empty()) throw std::invalid_argument("Model directory is required");
        const fs::path directory = fs::u8path(model_directory);
        if (!directory.is_absolute()) throw std::invalid_argument("Model directory must be absolute");
        const auto hotwords = BoundedUtf8(config->hotwords_utf8, 8192, "Invalid UTF-8 hotwords");
        for (const auto& file : ModelFiles) {
            const fs::path path = directory / file.name;
            if (!fs::is_regular_file(path) || fs::file_size(path) != file.size || FileHash(path) != file.sha256) {
                const std::string error = std::string("Pinned model file missing or SHA256 mismatch: ") + file.name;
                CopyError(error.c_str(), error_utf8, error_capacity);
                return DSR_MODEL_ERROR;
            }
        }
        const auto encoder = (directory / ModelFiles[0].name).u8string();
        const auto decoder = (directory / ModelFiles[1].name).u8string();
        const auto joiner = (directory / ModelFiles[2].name).u8string();
        const auto tokens = ReadFile(directory / ModelFiles[3].name);
        ValidateHotwords(hotwords, tokens);
        SherpaOnnxOnlineRecognizerConfig native{};
        native.feat_config.sample_rate = 16000;
        native.feat_config.feature_dim = 80;
        native.model_config.transducer.encoder = encoder.c_str();
        native.model_config.transducer.decoder = decoder.c_str();
        native.model_config.transducer.joiner = joiner.c_str();
        native.model_config.tokens_buf = tokens.data();
        native.model_config.tokens_buf_size = static_cast<int32_t>(tokens.size());
        native.model_config.num_threads = config->num_threads ? config->num_threads : 1;
        native.model_config.provider = "cpu";
        native.model_config.modeling_unit = "cjkchar";
        native.decoding_method = "modified_beam_search";
        native.max_active_paths = 4;
        native.enable_endpoint = 0;
        native.hotwords_score = config->hotword_score > 0 ? config->hotword_score : 2.0f;
        native.hotwords_buf = hotwords.data();
        native.hotwords_buf_size = static_cast<int32_t>(hotwords.size());
        auto handle = std::make_unique<DsrHandle>();
        handle->max_utterance_ms = config->max_utterance_ms ? config->max_utterance_ms : 15000;
        handle->recognizer = SherpaOnnxCreateOnlineRecognizer(&native);
        if (!handle->recognizer) {
            CopyError("Pinned recognizer could not be created", error_utf8, error_capacity);
            return DSR_MODEL_ERROR;
        }
        *out_handle = handle.release();
        return DSR_OK;
    } catch (const std::invalid_argument& error) {
        CopyError(error.what(), error_utf8, error_capacity);
        return DSR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        CopyError(error.what(), error_utf8, error_capacity);
    } catch (...) {
        CopyError("Unknown error creating speech runtime", error_utf8, error_capacity);
    }
    return DSR_RUNTIME_ERROR;
}

int32_t dsr_begin(DsrHandle* handle, uint64_t turn_id) {
    return Guard(handle, [&]() -> int32_t {
        if (turn_id == 0) return handle->Fail(DSR_INVALID_ARGUMENT, "Turn ID must be nonzero");
        if (turn_id < handle->turn) return handle->Fail(DSR_STALE_TURN, "Turn IDs must increase monotonically");
        if (handle->turn == turn_id)
            return handle->state == DSR_STREAMING ? DSR_OK : handle->Fail(DSR_INVALID_STATE, "Turn already ended");
        if (handle->state == DSR_STREAMING) return handle->Fail(DSR_INVALID_STATE, "Cancel or finish the active turn first");
        handle->ReleaseStream();
        handle->stream = SherpaOnnxCreateOnlineStream(handle->recognizer);
        if (!handle->stream) return handle->Fail(DSR_RUNTIME_ERROR, "Unable to create recognition stream");
        handle->turn = turn_id;
        handle->state = DSR_STREAMING;
        handle->samples = 0;
        handle->sample_rate = 0;
        handle->has_nonzero_input = false;
        handle->text.clear();
        handle->error.clear();
        ++handle->revision;
        return DSR_OK;
    });
}

int32_t dsr_push(DsrHandle* handle, uint64_t turn_id, int32_t sample_rate,
                 const float* samples, uint32_t sample_count) {
    return Guard(handle, [&]() -> int32_t {
        if (turn_id == 0 || turn_id != handle->turn) return handle->Fail(DSR_STALE_TURN, "Audio belongs to another turn");
        if (handle->state != DSR_STREAMING) return handle->Fail(DSR_INVALID_STATE, "Turn is not recording");
        if (!SupportedRate(sample_rate) || sample_count > static_cast<uint32_t>(sample_rate) ||
            (!samples && sample_count != 0) || (handle->sample_rate && handle->sample_rate != sample_rate))
            return handle->Fail(DSR_INVALID_ARGUMENT, "Invalid PCM buffer or sample rate changed within a turn");
        if ((handle->samples + sample_count) * 1000 > static_cast<uint64_t>(sample_rate) * handle->max_utterance_ms)
            return handle->Fail(DSR_LIMIT_EXCEEDED, "Utterance duration limit exceeded");
        for (uint32_t index = 0; index < sample_count; ++index)
            if (!std::isfinite(samples[index]) || samples[index] < -1.0f || samples[index] > 1.0f)
                return handle->Fail(DSR_INVALID_ARGUMENT, "PCM must contain finite normalized float samples");
        if (sample_count == 0) return DSR_OK;
        handle->sample_rate = sample_rate;
        handle->samples += sample_count;
        // 精确零样本也计入录音时长，首次非零前不送入模型，避免数字静音被补成台词
        uint32_t start = 0;
        if (!handle->has_nonzero_input) {
            while (start < sample_count && samples[start] == 0.0f) ++start;
            if (start == sample_count) return DSR_OK;
            handle->has_nonzero_input = true;
        }
        SherpaOnnxOnlineStreamAcceptWaveform(handle->stream, sample_rate, samples + start,
            static_cast<int32_t>(sample_count - start));
        handle->Refresh();
        return DSR_OK;
    });
}

int32_t dsr_finish(DsrHandle* handle, uint64_t turn_id) {
    return Guard(handle, [&]() -> int32_t {
        if (turn_id == 0 || turn_id != handle->turn) return handle->Fail(DSR_STALE_TURN, "Finish belongs to another turn");
        if (handle->state == DSR_FINAL) return DSR_OK;
        if (handle->state != DSR_STREAMING) return handle->Fail(DSR_INVALID_STATE, "Turn was cancelled or never started");
        // 空输入与精确数字静音返回空结果，噪声和回声仍需要采集链路处理
        if (handle->has_nonzero_input) {
            const std::vector<float> padding(static_cast<size_t>(handle->sample_rate * 66 / 100), 0.0f);
            SherpaOnnxOnlineStreamAcceptWaveform(handle->stream, handle->sample_rate,
                padding.data(), static_cast<int32_t>(padding.size()));
            SherpaOnnxOnlineStreamInputFinished(handle->stream);
            handle->Refresh();
        }
        handle->ReleaseStream();
        handle->state = DSR_FINAL;
        ++handle->revision;
        return DSR_OK;
    });
}

int32_t dsr_cancel(DsrHandle* handle, uint64_t turn_id) {
    return Guard(handle, [&]() -> int32_t {
        if (turn_id == 0 || turn_id != handle->turn) return handle->Fail(DSR_STALE_TURN, "Cancel belongs to another turn");
        if (handle->state == DSR_CANCELLED) return DSR_OK;
        handle->ReleaseStream();
        handle->text.clear();
        handle->state = DSR_CANCELLED;
        ++handle->revision;
        return DSR_OK;
    });
}

int32_t dsr_get_result(DsrHandle* handle, DsrResultInfo* info,
                       char* text_utf8, uint32_t capacity, uint32_t* required_capacity) {
    return Guard(handle, [&]() -> int32_t {
        if (!info || info->struct_size != sizeof(DsrResultInfo) || !required_capacity)
            return handle->Fail(DSR_INVALID_ARGUMENT, "Invalid result structure");
        info->state = handle->state;
        info->turn_id = handle->turn;
        info->revision = handle->revision;
        info->input_sample_count = handle->samples;
        info->input_sample_rate = handle->sample_rate;
        info->reserved = 0;
        return CopyText(handle->text, text_utf8, capacity, required_capacity);
    });
}

int32_t dsr_get_error(DsrHandle* handle, char* error_utf8, uint32_t capacity, uint32_t* required_capacity) {
    return Guard(handle, [&]() -> int32_t {
        if (!required_capacity) return DSR_INVALID_ARGUMENT;
        return CopyText(handle->error, error_utf8, capacity, required_capacity);
    });
}

void dsr_destroy(DsrHandle* handle) {
    try { delete handle; } catch (...) {}
}
