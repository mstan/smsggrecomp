include_guard(GLOBAL)

# The game owns game.toml and generated artifacts; this framework owns every
# build input and generation step shared by SMS/GG hosts.
function(sms_add_game TARGET)
    set(_engine "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(_config "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")
    file(READ "${_config}" _text)
    string(REGEX MATCH "output_prefix[ \t]*=[ \t]*\"([A-Za-z_][A-Za-z_0-9]*)\"" _match "${_text}")
    set(_prefix "${CMAKE_MATCH_1}")
    if(NOT _prefix)
        message(FATAL_ERROR "game.toml requires a C-identifier output_prefix")
    endif()
    string(REGEX MATCH "rom[ \t]*=[ \t]*\"([^\"]+)\"" _match "${_text}")
    set(SMSGG_ROM "${CMAKE_CURRENT_SOURCE_DIR}/${CMAKE_MATCH_1}" CACHE FILEPATH "Owned cartridge ROM")
    option(SMSGG_REGENERATE "Regenerate native code from the verified ROM" ON)
    option(SMSGG_ENABLE_SDL "Build the interactive SDL2 host" ON)
    set(_gen "${CMAKE_CURRENT_SOURCE_DIR}/generated")
    set(_sources "${_gen}/${_prefix}_full.c" "${_gen}/${_prefix}_dispatch.c" "${_gen}/${_prefix}_layout.c")
    if(SMSGG_REGENERATE)
        if(NOT EXISTS "${SMSGG_ROM}")
            message(FATAL_ERROR "Set SMSGG_ROM to your supported extracted cartridge")
        endif()
        add_subdirectory("${_engine}/recompiler" "${CMAKE_CURRENT_BINARY_DIR}/sms-recompiler")
        set(_deps "${SMSGG_ROM}" "${_config}")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dispatch_manifest.txt")
            list(APPEND _deps "${CMAKE_CURRENT_SOURCE_DIR}/dispatch_manifest.txt")
        endif()
        add_custom_command(OUTPUT ${_sources}
            BYPRODUCTS "${_gen}/${_prefix}_funcs.h"
            COMMAND "$<TARGET_FILE:SmsRecomp>" "${SMSGG_ROM}" --game "${_config}"
            DEPENDS SmsRecomp ${_deps}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
    else()
        foreach(_source IN LISTS _sources)
            if(NOT EXISTS "${_source}")
                message(FATAL_ERROR "Missing generated source ${_source}; enable SMSGG_REGENERATE")
            endif()
        endforeach()
    endif()
    add_executable(${TARGET} ${_sources}
        "${_engine}/runner/main.c" "${_engine}/runner/glue.c"
        "${_engine}/runner/coverage_route.c"
        "${_engine}/runner/video/sms_vdp.c" "${_engine}/runner/audio/sn76489.c"
        "${_engine}/runner/external/superzazu/z80.c")
    target_compile_features(${TARGET} PRIVATE c_std_11)
    target_compile_definitions(${TARGET} PRIVATE SMSGG_HAVE_GAME_LAYOUT)
    target_include_directories(${TARGET} PRIVATE "${_engine}/runner" "${_engine}/runner/include" "${_gen}")
    if(SMSGG_ENABLE_SDL)
        find_package(SDL2 CONFIG REQUIRED)
        target_sources(${TARGET} PRIVATE "${_engine}/runner/host_sdl.c")
        target_compile_definitions(${TARGET} PRIVATE SMS_HAVE_SDL)
        target_link_libraries(${TARGET} PRIVATE SDL2::SDL2)
        if(WIN32)
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:${TARGET}>"
                VERBATIM)
        endif()
    endif()
endfunction()
