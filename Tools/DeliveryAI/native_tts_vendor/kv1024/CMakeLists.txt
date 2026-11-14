cmake_minimum_required(VERSION 3.14...3.28)
project(DeliveryQwenKv1024 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)
if(NOT MSVC OR NOT QWEN_SOURCE_DIR OR NOT QWEN_PREPARED_DIR OR NOT QWEN_BASE_BUILD_DIR)
    message(FATAL_ERROR "Use the prepared Windows native Qwen build")
endif()
# 复用已验证的 CUDA 后端，只编译改变缓存容量的原生语音 DLL
add_library(qwen SHARED
    "${QWEN_SOURCE_DIR}/src/qwen.cpp"
    "${QWEN_PREPARED_DIR}/pipeline-tts.cpp"
    "${QWEN_SOURCE_DIR}/src/pipeline-codec.cpp")
target_compile_definitions(qwen PRIVATE QWEN_BUILD GGML_SHARED GGML_BACKEND_SHARED
    GGML_USE_CPU GGML_USE_CUDA GGML_MAX_NAME=128 _CRT_SECURE_NO_WARNINGS)
target_compile_options(qwen PRIVATE /utf-8 /W4 /wd4100 /wd4505)
target_include_directories(qwen PRIVATE "${QWEN_PREPARED_DIR}" "${QWEN_SOURCE_DIR}/src" "${QWEN_SOURCE_DIR}")
target_include_directories(qwen SYSTEM PRIVATE "${QWEN_SOURCE_DIR}/ggml/include")
target_link_libraries(qwen PRIVATE Threads::Threads
    "${QWEN_BASE_BUILD_DIR}/ggml/src/ggml.lib"
    "${QWEN_BASE_BUILD_DIR}/ggml/src/ggml-base.lib"
    "${QWEN_BASE_BUILD_DIR}/ggml/src/ggml-cpu.lib"
    "${QWEN_BASE_BUILD_DIR}/ggml/src/ggml-cuda/ggml-cuda.lib")
