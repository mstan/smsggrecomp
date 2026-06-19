cmake_minimum_required(VERSION 3.16)
project(SmsRecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(SmsRecomp
    src/main_sms.c
    src/rom_parser.c
    src/z80_decoder.c
    src/function_finder.c
    src/game_config.c
    src/code_generator.c
    src/toml.c
)

if(MSVC)
    target_compile_definitions(SmsRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(SmsRecomp PRIVATE /W3)
else()
    target_compile_options(SmsRecomp PRIVATE -Wall -Wextra)
endif()
