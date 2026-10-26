# Unit tests for the pure, platform-independent pieces of the engine
# (containers and DSP). No Windows audio API is touched here, so these run
# on any MSVC toolchain -- including the CI runner -- in a fraction of a second.

function(audiolinker_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE NOMINMAX)
    target_compile_options(${name} PRIVATE /utf-8)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

audiolinker_test(test_ringbuffer)
audiolinker_test(test_resampler)
audiolinker_test(test_biquad)
