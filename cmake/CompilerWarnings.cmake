function(airplaywin_set_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /EHsc
            /utf-8
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wsign-conversion
        )
    endif()
endfunction()
