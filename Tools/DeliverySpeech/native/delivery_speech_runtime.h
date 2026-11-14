#pragma once

#include <stdint.h>

#if defined(_WIN32) && defined(DELIVERY_SPEECH_RUNTIME_EXPORTS)
#define DSR_API __declspec(dllexport)
#else
#define DSR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DSR_ABI_VERSION 1u

typedef struct DsrHandle DsrHandle;

typedef enum DsrStatus {
    DSR_OK = 0,
    DSR_INVALID_ARGUMENT = 1,
    DSR_INVALID_STATE = 2,
    DSR_STALE_TURN = 3,
    DSR_BUFFER_TOO_SMALL = 4,
    DSR_MODEL_ERROR = 5,
    DSR_RUNTIME_ERROR = 6,
    DSR_LIMIT_EXCEEDED = 7
} DsrStatus;

typedef enum DsrState {
    DSR_IDLE = 0,
    DSR_STREAMING = 1,
    DSR_FINAL = 2,
    DSR_CANCELLED = 3
} DsrState;

// 每个句柄由一个识别工作线程串行调用，不在采音回调或游戏线程中推理
// 字符串都是 UTF-8，创建时复制配置，调用者可在返回后释放这些字符串
typedef struct DsrConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* model_directory_utf8;
    const char* hotwords_utf8;
    uint32_t num_threads;            // 0 使用默认 1，最多 2
    uint32_t max_utterance_ms;       // 0 使用默认 15000，上限 30000
    float hotword_score;            // 0 使用默认 2，允许范围 0 到 5
    uint32_t reserved;              // 必须为 0
} DsrConfig;

// struct_size 由调用者填写，revision 在文字或状态变化时增加
typedef struct DsrResultInfo {
    uint32_t struct_size;
    uint32_t state;
    uint64_t turn_id;
    uint64_t revision;
    uint64_t input_sample_count;
    int32_t input_sample_rate;
    uint32_t reserved;
} DsrResultInfo;

DSR_API uint32_t dsr_abi_version(void);

// 创建前核对固定模型的尺寸和 SHA256，错误信息写入调用者缓冲
// out_handle 失败时为 NULL，error_capacity 包含结尾零字符
DSR_API int32_t dsr_create(const DsrConfig* config, DsrHandle** out_handle,
                          char* error_utf8, uint32_t error_capacity);

// turn_id 必须非 0，同一正在录入的轮次重复 begin 不会清空已有音频
// 新轮次必须等上一轮结束或取消后开始，已结束的编号不能再次开始
DSR_API int32_t dsr_begin(DsrHandle* handle, uint64_t turn_id);

// 单声道 float PCM 范围为 [-1, 1]，同一轮采样率不可变化
// 每批最多 1 秒音频，超过长度上限时不接收该批，调用者应取消本轮
DSR_API int32_t dsr_push(DsrHandle* handle, uint64_t turn_id, int32_t sample_rate,
                        const float* samples, uint32_t sample_count);

// 排空剩余音频并直接补入 0.66 秒零样本，不进行实际等待
// 同一 final 重复 finish 成功，cancel 后不能恢复 final
DSR_API int32_t dsr_finish(DsrHandle* handle, uint64_t turn_id);

// 取消清除本轮文字并释放 stream，重复取消允许，旧轮次不能影响新轮次
DSR_API int32_t dsr_cancel(DsrHandle* handle, uint64_t turn_id);

// required_capacity 始终包含结尾零字符，容量不足时返回 DSR_BUFFER_TOO_SMALL
// 此时 info 仍有效，调用者按 required_capacity 分配后再次复制
// 返回字符串由调用者持有，无需跨 DLL 释放
DSR_API int32_t dsr_get_result(DsrHandle* handle, DsrResultInfo* info,
                              char* text_utf8, uint32_t capacity,
                              uint32_t* required_capacity);

DSR_API int32_t dsr_get_error(DsrHandle* handle, char* error_utf8,
                             uint32_t capacity, uint32_t* required_capacity);

// NULL 允许，释放前调用者必须停止所有其他句柄调用
DSR_API void dsr_destroy(DsrHandle* handle);

#ifdef __cplusplus
}
#endif
