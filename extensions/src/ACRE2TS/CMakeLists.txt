cmake_minimum_required (VERSION 3.10)

set(ACRE_NAME "ACRE2TS")

acre_set_build_output()
acre_set_linker_options()

# MASM is MSVC-only. Cross-compiling with mingw substitutes api_compat_gcc.cpp,
# which reimplements api_compat.asm's trampoline in C++.
if(MSVC)
    enable_language(ASM_MASM)
    file(GLOB_RECURSE SOURCES *.h *.hpp *.c *.cpp *.asm inc/*)
else()
    file(GLOB_RECURSE SOURCES *.h *.hpp *.c *.cpp inc/*)
endif()

include_directories(inc)

add_library( ${ACRE_NAME} MODULE ${SOURCES} ${GLOBAL_SOURCES})
target_link_libraries(${ACRE_NAME} ACRE2Core ACRE2Shared x3daudio)
if(NOT MSVC)
    # mingw ignores #pragma comment(lib, ...), so name these explicitly.
    # ole32/mmdevapi are for the ambient WASAPI process-loopback source. Nothing
    # references it yet, so the linker does not pull that object out of
    # ACRE2Core and the build links without them -- until it is wired in, at
    # which point the failure would look unrelated to this file.
    target_link_libraries(${ACRE_NAME} shlwapi version ws2_32 advapi32 ole32 mmdevapi)
endif()
set_target_properties(${ACRE_NAME} PROPERTIES FOLDER ACRE2)
if(MSVC)
    set_target_properties(${ACRE_NAME} PROPERTIES LINK_FLAGS -SAFESEH:NO)
endif()
target_compile_features(${ACRE_NAME} PRIVATE cxx_std_17)


# Copy and rename
if(USE_64BIT_BUILD)
    set(FINAL_DLL_NAME acre2_win64.dll)
else()
    set(FINAL_DLL_NAME acre2_win32.dll)
endif()

add_custom_command(TARGET ${ACRE_NAME} POST_BUILD
    # Copy DLL to plugins
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${ACRE_NAME}> ${PROJECT_SOURCE_DIR}/../plugin/${FINAL_DLL_NAME}
)

# Only MSVC emits a .pdb.
if(MSVC)
    add_custom_command(TARGET ${ACRE_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:${ACRE_NAME}>/${ACRE_NAME}.pdb ${PROJECT_SOURCE_DIR}/../symbols/${ACRE_ARCH}/${ACRE_NAME}.pdb
    )
endif()
