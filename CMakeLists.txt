cmake_minimum_required(VERSION 3.26.0 FATAL_ERROR)

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD 20 CACHE STRING "The C++ standard to use")

set(GAME_STR "MM")
project(2s2h VERSION 5.0.1 LANGUAGES C CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "This fork supports Windows builds only.")
endif()
include(CMake/2ship-cvars.cmake)
include(CMake/lus-cvars.cmake)
set(SPDLOG_LEVEL_TRACE 0)
set(SPDLOG_LEVEL_OFF 6)
set(SPDLOG_MIN_CUTOFF SPDLOG_LEVEL_TRACE CACHE STRING "cutoff at trace")
set(PROJECT_BUILD_NAME "Battler Bravo" CACHE STRING "" FORCE)
set(PROJECT_TEAM "github.com/harbourmasters" CACHE STRING "" FORCE)

execute_process(
    COMMAND git branch --show-current
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CMAKE_PROJECT_GIT_BRANCH "${GIT_BRANCH}" CACHE STRING "Git branch" FORCE)

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get only the first 7 characters of the hash
string(SUBSTRING "${GIT_COMMIT_HASH}" 0 7 SHORT_COMMIT_HASH)

set(CMAKE_PROJECT_GIT_COMMIT_HASH "${SHORT_COMMIT_HASH}" CACHE STRING "Git commit hash" FORCE)

execute_process(
    COMMAND git describe --tags --abbrev=0 --exact-match HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT GIT_COMMIT_TAG)
    set(GIT_COMMIT_TAG "" CACHE STRING "Git commit tag" FORCE)
endif()

set(CMAKE_PROJECT_GIT_COMMIT_TAG "${GIT_COMMIT_TAG}" CACHE STRING "Git commit tag" FORCE)

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT 2ship)

add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if (CMAKE_C_COMPILER_LAUNCHER MATCHES "ccache|sccache")
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
    endif()
endif()

# Enable the Gfx debugger in LUS to use libgfxd from ZAPDTR
set(GFX_DEBUG_DISASSEMBLER ON)

# Tell LUS we're using F3DEX_GBI_2 (in a way that doesn't break libgfxd)
set(GBI_UCODE F3DEX_GBI_2)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# Enable MPQ and OTR support
set(INCLUDE_MPQ_SUPPORT ON)

################################################################################
# Set target arch type if empty. Visual studio solution generator provides it.
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT CMAKE_VS_PLATFORM_NAME)
        set(CMAKE_VS_PLATFORM_NAME "x64")
    endif()
    message("${CMAKE_VS_PLATFORM_NAME} architecture in use")

    if(NOT "${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
        message(FATAL_ERROR "${CMAKE_VS_PLATFORM_NAME} arch is not supported!")
    endif()
endif()

################################################################################
# VCPKG dependencies for Windows
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
include(CMake/automate-vcpkg.cmake)

set(VCPKG_TRIPLET x64-windows-static)
set(VCPKG_TARGET_TRIPLET x64-windows-static)

vcpkg_bootstrap()
vcpkg_install_packages(zlib bzip2 libzip libpng sdl2 sdl2-net glew glfw3 nlohmann-json tinyxml2 spdlog libogg libvorbis opus opusfile)
endif()

################################################################################
# Global configuration types
################################################################################
if(MSVC)
    # We need to remove RTC1 as it is not compatible with O2, which we set for some files in DEBUG
    string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
endif()

if(NOT CMAKE_BUILD_TYPE )
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()

################################################################################
# Common utils
################################################################################
include(CMake/Utils.cmake)

message(STATUS "Windows x64")

################################################################################
# Additional Global Settings(add specific info there)
################################################################################
include(CMake/GlobalSettingsInclude.cmake OPTIONAL)

if(WIN32)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build-artifacts/lib")
    set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build-artifacts/pdb")
endif()

################################################################################
# Use solution folders feature
################################################################################
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

################################################################################
# Set CONTROLLERBUTTONS_T
################################################################################
add_compile_definitions(CONTROLLERBUTTONS_T=uint32_t)

################################################################################
# Sub-projects
################################################################################
add_subdirectory(libultraship ${CMAKE_BINARY_DIR}/libultraship)
target_compile_definitions(libultraship PUBLIC INCLUDE_MPQ_SUPPORT)
# LUS ships no PCH of its own and these headers dominate its build. Done here rather than in the
# submodule, and PRIVATE, so nothing about its public interface changes.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU|MSVC")
    target_precompile_headers(libultraship PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:<ship/Context.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<ship/resource/ResourceManager.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<ship/window/Window.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<spdlog/spdlog.h$<ANGLE-R>>"
    )
    # Clang replays a PCH's pending template instantiations in every consuming TU unless told to do
    # the work up front. MSVC's PCH already behaves this way.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(libultraship PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fpch-instantiate-templates>
        )
    endif()
endif()

add_subdirectory(ZAPDTR/ZAPD ${CMAKE_BINARY_DIR}/ZAPD)
set_target_properties(ZAPD PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/build-artifacts/bin")
add_subdirectory(OTRExporter)
if(WIN32)
    set(ML64_CONVERTER_EXE "${CMAKE_BINARY_DIR}/pak_converter/ML64PakConverter.exe")
endif()
add_subdirectory(mm)
set_target_properties(libultraship OTRExporter ZAPDLib PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/build-artifacts/lib")

install(DIRECTORY "${CMAKE_SOURCE_DIR}/mm/assets/extractor/" DESTINATION ./assets/extractor COMPONENT 2s2h)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/mm/assets/xml/" DESTINATION ./assets/xml COMPONENT 2s2h)


find_package(Python3 COMPONENTS Interpreter)

if(WIN32)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "Python 3 is required to build the ModLoader64 .pak converter.")
    endif()

    file(GLOB_RECURSE ML64_CONVERTER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/ml64pak_converter/*.py"
        "${CMAKE_SOURCE_DIR}/tools/ml64pak_converter/*.txt"
        "${CMAKE_SOURCE_DIR}/tools/ml64pak_converter/*.md")
    add_custom_command(
        OUTPUT "${ML64_CONVERTER_EXE}"
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/build_ml64pak_converter.ps1"
            -OutputPath "${ML64_CONVERTER_EXE}"
        DEPENDS
            ${ML64_CONVERTER_SOURCES}
            "${CMAKE_SOURCE_DIR}/tools/build_ml64pak_converter.ps1"
        COMMENT "Building the ModLoader64 .pak converter..."
        VERBATIM)
    add_custom_target(BuildML64PakConverter
        DEPENDS "${ML64_CONVERTER_EXE}"
        COMMENT "Building the ModLoader64 converter...")
    add_dependencies(2ship BuildML64PakConverter)
endif()

# Target to generate OTRs
add_custom_target(
    ExtractAssets
    # CMake versions prior to 3.17 do not have the rm command, use remove instead for older versions
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<VERSION_LESS:${CMAKE_VERSION},3.17>,remove,rm> -f mm.zip 2ship.o2r
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/OTRExporter/extract_assets.py -z "$<TARGET_FILE:ZAPD>" --non-interactive --xml-root ../mm/assets/xml --custom-otr-file 2ship.o2r "--custom-assets-path" ${CMAKE_CURRENT_SOURCE_DIR}/mm/assets/custom --port-ver "${CMAKE_PROJECT_VERSION}"
    COMMAND ${CMAKE_COMMAND} -DSYSTEM_NAME=${CMAKE_SYSTEM_NAME} -DTARGET_DIR="$<TARGET_FILE_DIR:ZAPD>" -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/copy-existing-otrs.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/mm
    COMMENT "Running asset extraction..."
    DEPENDS ZAPD
    BYPRODUCTS mm.o2r ${CMAKE_SOURCE_DIR}/mm.o2r ${CMAKE_SOURCE_DIR}/2ship.o2r
)

# Target to generate headers
add_custom_target(
    ExtractAssetHeaders
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/OTRExporter/extract_assets.py -z "$<TARGET_FILE:ZAPD>" --non-interactive --xml-root ../mm/assets/xml --gen-headers
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/mm
    COMMENT "Generating asset headers..."
    DEPENDS ZAPD
)

# Target to generate only 2ship.o2r
add_custom_target(
    Generate2ShipOtr
    # CMake versions prior to 3.17 do not have the rm command, use remove instead for older versions
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<VERSION_LESS:${CMAKE_VERSION},3.17>,remove,rm> -f 2ship.o2r
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/OTRExporter/extract_assets.py -z "$<TARGET_FILE:ZAPD>" --norom --custom-otr-file 2ship.o2r "--custom-assets-path" ${CMAKE_CURRENT_SOURCE_DIR}/mm/assets/custom --port-ver "${CMAKE_PROJECT_VERSION}"
    COMMAND ${CMAKE_COMMAND} -DSYSTEM_NAME=${CMAKE_SYSTEM_NAME} -DTARGET_DIR="$<TARGET_FILE_DIR:ZAPD>" -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR} -DONLY2SHIPOTR=On -P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/copy-existing-otrs.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/mm
    COMMENT "Generating 2ship.o2r..."
    DEPENDS ZAPD
)

install(FILES ${CMAKE_SOURCE_DIR}/README.md DESTINATION . COMPONENT 2s2h)
if(WIN32)
    install(FILES
        "${ML64_CONVERTER_EXE}"
        "${CMAKE_SOURCE_DIR}/tools/ml64pak_converter/UPSTREAM-LICENSE"
        "${CMAKE_SOURCE_DIR}/tools/ml64pak_converter/NOTICE.md"
        DESTINATION tools COMPONENT 2s2h)
endif()
install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/mods\")" COMPONENT 2s2h)
install(CODE "file(TOUCH \"\${CMAKE_INSTALL_PREFIX}/mods/custom_mod_files_go_here.txt\")" COMPONENT 2s2h)

set(CPACK_GENERATOR "ZIP")

set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_SOURCE_DIR}/CMake/Packaging-2.cmake)
include(CMake/Packaging.cmake)
