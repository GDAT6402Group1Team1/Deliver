cmake_minimum_required(VERSION 3.24)
project(DeliverySpeechRuntime LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DeliverySpeechRuntime v1 requires Windows x64 and MSVC")
endif()
if(NOT EXISTS "${SHERPA_ROOT}/include/sherpa-onnx/c-api/c-api.h")
    message(FATAL_ERROR "Run prepare_runtime.py and supply SHERPA_ROOT")
endif()

add_library(DeliverySpeechRuntime SHARED delivery_speech_runtime.cpp)
target_compile_features(DeliverySpeechRuntime PRIVATE cxx_std_17)
target_compile_definitions(DeliverySpeechRuntime PRIVATE DELIVERY_SPEECH_RUNTIME_EXPORTS)
target_compile_options(DeliverySpeechRuntime PRIVATE /utf-8 /W4 /EHsc)
set_property(TARGET DeliverySpeechRuntime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_include_directories(DeliverySpeechRuntime PRIVATE "${SHERPA_ROOT}/include")

# 官方固定版本包里的各库静态链接在本 DLL 内，不依赖同名 onnxruntime.dll
foreach(NAME sherpa-onnx-c-api sherpa-onnx-core sherpa-onnx-kaldifst-core
             sherpa-onnx-fstfar sherpa-onnx-fst kaldi-decoder-core
             kaldi-native-fbank-core kissfft-float ssentencepiece_core utf8proc onnxruntime)
    if(NOT EXISTS "${SHERPA_ROOT}/lib/${NAME}.lib")
        message(FATAL_ERROR "Missing static dependency: ${NAME}.lib")
    endif()
    target_link_libraries(DeliverySpeechRuntime PRIVATE "${SHERPA_ROOT}/lib/${NAME}.lib")
endforeach()
target_link_libraries(DeliverySpeechRuntime PRIVATE bcrypt ws2_32 dbghelp shlwapi version psapi advapi32 ole32)
set_target_properties(DeliverySpeechRuntime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DELIVERY_RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${DELIVERY_RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
