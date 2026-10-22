# Common compile options applied through dc_apply_options(<target>).

if(WIN32)
    # Windows 10 or later. Keeps <windows.h> lean and avoids min/max macros.
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
                            _CRT_SECURE_NO_WARNINGS)
endif()

function(dc_apply_options target)
    if(MSVC)
        # C4324: structure padded due to alignas (intended for the shared memory layout)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /wd4324)
        if(DC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
        if(DC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
