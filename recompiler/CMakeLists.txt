cmake_minimum_required(VERSION 3.16)
project(SmsRecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(Z80_RECOMP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/z80-recomp-core")
if(NOT TARGET z80-recomp-core::frontend)
    add_subdirectory("${Z80_RECOMP_CORE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/z80-recomp-core")
endif()

add_executable(SmsRecomp
    src/main_sms.c
    src/rom_parser.c
    src/function_finder.c
    src/game_config.c
    src/code_generator.c
    src/toml.c
)
target_link_libraries(SmsRecomp PRIVATE z80-recomp-core::frontend)

if(MSVC)
    target_compile_definitions(SmsRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(SmsRecomp PRIVATE /W3)
else()
    target_compile_options(SmsRecomp PRIVATE -Wall -Wextra)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(sms_decoder_selftest ../tests/z80_decoder_selftest.c)
        target_link_libraries(sms_decoder_selftest PRIVATE z80-recomp-core::frontend)
        add_test(NAME sms_decoder COMMAND sms_decoder_selftest)
        add_executable(sms_frontend_selftest ../tests/frontend_selftest.c
            src/function_finder.c src/rom_parser.c)
        target_include_directories(sms_frontend_selftest PRIVATE src)
        target_link_libraries(sms_frontend_selftest PRIVATE z80-recomp-core::frontend)
        add_test(NAME sms_frontend COMMAND sms_frontend_selftest)

        add_executable(sms_bank_generate ../tests/generator_bank_selftest.c
            src/code_generator.c src/function_finder.c src/rom_parser.c)
        target_compile_definitions(sms_bank_generate PRIVATE SMS_TEST_GENERATE=1)
        target_include_directories(sms_bank_generate PRIVATE src)
        target_link_libraries(sms_bank_generate PRIVATE z80-recomp-core::frontend)
        set(bank_fixture "${CMAKE_CURRENT_BINARY_DIR}/bank-fixture")
        file(MAKE_DIRECTORY "${bank_fixture}")
        add_custom_command(
            OUTPUT "${bank_fixture}/bankfixture_full.c" "${bank_fixture}/bankfixture_dispatch.c"
                   "${bank_fixture}/bankfixture_layout.c" "${bank_fixture}/bankfixture_funcs.h"
            COMMAND sms_bank_generate "${bank_fixture}"
            DEPENDS sms_bank_generate VERBATIM)
        add_custom_target(sms_bank_fixture DEPENDS "${bank_fixture}/bankfixture_full.c")
        add_executable(sms_bank_selftest ../tests/generator_bank_selftest.c
            "${bank_fixture}/bankfixture_full.c" "${bank_fixture}/bankfixture_dispatch.c")
        target_compile_definitions(sms_bank_selftest PRIVATE SMS_NATIVE_HOOKS=1)
        target_include_directories(sms_bank_selftest PRIVATE ../runner/include "${bank_fixture}")
        add_test(NAME sms_bank COMMAND sms_bank_selftest)
        add_executable(sms_native_spans_selftest ../tests/native_spans_selftest.c)
        add_dependencies(sms_native_spans_selftest sms_bank_fixture)
        target_compile_definitions(sms_native_spans_selftest PRIVATE SMS_NATIVE_HOOKS=1)
        target_include_directories(sms_native_spans_selftest PRIVATE ../runner/include "${bank_fixture}")
        add_test(NAME sms_native_spans COMMAND sms_native_spans_selftest)
    endif()
endif()
