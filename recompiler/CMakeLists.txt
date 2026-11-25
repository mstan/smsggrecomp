cmake_minimum_required(VERSION 3.16)
project(SmsRecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(SmsRecomp
    src/main_sms.c
    src/rom_parser.c
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

set(RECOMP_SHARED_CORE_ROOT "" CACHE PATH "Frozen private shared-core integration workspace")
function(sms_shared_frontend)
  set(BUILD_TESTING OFF)
  add_subdirectory("${RECOMP_SHARED_CORE_ROOT}/z80-recomp-core" "${CMAKE_BINARY_DIR}/shared-z80" EXCLUDE_FROM_ALL)
endfunction()
sms_shared_frontend()
target_link_libraries(SmsRecomp PRIVATE z80-recomp-core::frontend)
