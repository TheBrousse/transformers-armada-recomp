# The exe's icon (TODO 198): Optimus Prime's head from the main menu's backdrop, made from the
# player's own disc by tools/make_exe_icon.py into the build folder (never into the repository) and
# embedded through a .rc file. Without the disc's files (or Python with Pillow) the exe is built with
# no icon of its own; the next configure after the disc is extracted makes it (build.ps1 configures
# at every build). Included from CMakeLists.txt after the TransformersRecomp target exists.

if(NOT WIN32)
    return()
endif()

set(TF_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "The disc's extracted files (build.ps1 extracts them there)")
set(TF_ICON_DIR "${CMAKE_CURRENT_BINARY_DIR}/tf_icon")
set(TF_ICON "${TF_ICON_DIR}/transformers.ico")
find_program(TF_ICON_PYTHON NAMES python python3)

if(TF_ICON_PYTHON AND EXISTS "${TF_GAME_DIR}/TFA.ZIP")
    # (written only when it changes: the same disc gives the same file, nothing is compiled again)
    execute_process(
        COMMAND "${TF_ICON_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_exe_icon.py" --game "${TF_GAME_DIR}" --out "${TF_ICON}"
        RESULT_VARIABLE tf_icon_result
        OUTPUT_VARIABLE tf_icon_out
        ERROR_VARIABLE tf_icon_err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT tf_icon_result EQUAL 0)
        message(STATUS "TransformersRecomp: no exe icon (${tf_icon_out}${tf_icon_err})")
    endif()
endif()

if(EXISTS "${TF_ICON}")
    enable_language(RC)
    file(CONFIGURE OUTPUT "${TF_ICON_DIR}/transformers.rc"
         CONTENT "// Written by transformers/icon.cmake: do not edit.\n1 ICON \"@TF_ICON@\"\n" @ONLY)
    target_sources(TransformersRecomp PRIVATE "${TF_ICON_DIR}/transformers.rc")
    set_source_files_properties("${TF_ICON_DIR}/transformers.rc" PROPERTIES OBJECT_DEPENDS "${TF_ICON}")
    message(STATUS "TransformersRecomp: exe icon from the disc (${TF_ICON})")
endif()
