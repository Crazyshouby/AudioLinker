# Unit tests for the pure, platform-independent pieces of the engine
# (containers and DSP) and the config (de)serialization. No audio API is
# touched here, so these run on any MSVC toolchain -- including the CI
# runner -- in a fraction of a second.

function(audiolinker_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party/json ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(${name} PRIVATE /utf-8)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

audiolinker_test(test_ringbuffer)
audiolinker_test(test_resampler)
audiolinker_test(test_biquad)
audiolinker_test(test_config ${CMAKE_SOURCE_DIR}/src/Config.cpp ${CMAKE_SOURCE_DIR}/src/Log.cpp)
