# The version a build is made from (TODO 104): run at every build (cmake -P, the tf_version target in
# CMakeLists.txt), it writes tf_version.h only when its contents change, so a build of the same commit
# compiles nothing again.
#
#   -DSOURCE=<the port folder>  -DOUTPUT=<the header to write>
#
# TF_VERSION_COMMIT  the commit the source is at ("" when git is not there)
# TF_VERSION_KIND    "install": an installed copy, so the game can check for and install updates:
#                    a clone of the game's repository set up by setup.ps1 / tfa-setup.exe (update.ps1
#                    and setup.ps1's .setup.json at the port folder, which is the clone's top), or the
#                    port\ of a repository that holds the port folder (update.ps1 and port\ at its
#                    top); "dev": a development checkout (never set up with setup.ps1).
# TF_VERSION_BRANCH  its branch; TF_VERSION_DATE its date (YYYY-MM-DD); TF_VERSION_LOCAL 1 when the
#                    port folder has changes of its own that are not committed.
# TF_VERSION_NAME    the version's number from the VERSION file at the port folder's top (TODO 111:
#                    "0.1.3"; port/VERSION in the home repository, bumped by one at every release to
#                    main); "" when there is no such file or it does not read as a number like 0.1.3.

set(commit "")
set(kind "dev")
set(branch "")
set(date "")
set(local 0)
find_program(GIT_EXE git)
if(GIT_EXE)
    execute_process(COMMAND "${GIT_EXE}" -C "${SOURCE}" rev-parse HEAD
                    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE r1)
    if(NOT r1 EQUAL 0)
        set(commit "")
    endif()
    if(commit)
        execute_process(COMMAND "${GIT_EXE}" -C "${SOURCE}" rev-parse --abbrev-ref HEAD
                        OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXE}" -C "${SOURCE}" log -1 --format=%cs
                        OUTPUT_VARIABLE date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXE}" -C "${SOURCE}" rev-parse --show-toplevel
                        OUTPUT_VARIABLE top OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        # (the home repository: update.ps1 and port\ at its top, this folder being that port\)
        get_filename_component(source_real "${SOURCE}" REALPATH)
        if(top AND EXISTS "${top}/update.ps1" AND EXISTS "${top}/port")
            get_filename_component(port_real "${top}/port" REALPATH)
            if(port_real STREQUAL source_real)
                set(kind "install")
            endif()
        endif()
        # (a clone of the game's own repository: this folder is its top, set up by setup.ps1)
        if(kind STREQUAL "dev" AND top AND EXISTS "${SOURCE}/update.ps1" AND EXISTS "${SOURCE}/.setup.json")
            get_filename_component(top_real "${top}" REALPATH)
            if(top_real STREQUAL source_real)
                set(kind "install")
            endif()
        endif()
        # (the PS2Recomp submodule always has the port's patches applied: not a local change)
        execute_process(COMMAND "${GIT_EXE}" -C "${SOURCE}" status --porcelain --untracked-files=no --ignore-submodules -- .
                        OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(changes)
            set(local 1)
        endif()
    endif()
endif()

# (the number: the port folder's VERSION file; in an install that is port\VERSION of the copy)
set(name "")
if(EXISTS "${SOURCE}/VERSION")
    file(STRINGS "${SOURCE}/VERSION" name_lines LIMIT_COUNT 1)
    if(name_lines)
        list(GET name_lines 0 name)
        string(STRIP "${name}" name)
    endif()
    if(NOT name MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
        set(name "")
    endif()
endif()

# A build made elsewhere for a copy of the repository (prebuilt binaries, a test) names its commit and
# kind: -DTF_VERSION_COMMIT_OVERRIDE=<sha> -DTF_VERSION_KIND_OVERRIDE=install (CMake cache variables of
# the same names, passed on by CMakeLists.txt).
if(COMMIT_OVERRIDE)
    set(commit "${COMMIT_OVERRIDE}")
    set(local 0)
endif()
if(KIND_OVERRIDE)
    set(kind "${KIND_OVERRIDE}")
endif()

set(text "// Written by transformers/version.cmake at every build: do not edit.\n#pragma once\n")
string(APPEND text "#define TF_VERSION_COMMIT \"${commit}\"\n")
string(APPEND text "#define TF_VERSION_KIND \"${kind}\"\n")
string(APPEND text "#define TF_VERSION_BRANCH \"${branch}\"\n")
string(APPEND text "#define TF_VERSION_DATE \"${date}\"\n")
string(APPEND text "#define TF_VERSION_LOCAL ${local}\n")
string(APPEND text "#define TF_VERSION_NAME \"${name}\"\n")

set(old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUTPUT}" "${text}")
endif()
