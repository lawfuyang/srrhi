cmake_minimum_required(VERSION 3.16)
project(srrhi)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Speed up MSVC builds by enabling multi-processor compilation.
if(MSVC)
    add_compile_options(/MP)
endif()

# Output all executables to /bin/ directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin)

function(download_library LIB_NAME LIB_VERSION DOWNLOAD_URL LIB_PATTERN)
    set(LIB_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/${LIB_NAME}")
    set(LIB_VERSION_FILE "${LIB_INSTALL_DIR}/version.txt")

    # Ensure the library directory exists before any further operations
    if(NOT EXISTS "${LIB_INSTALL_DIR}")
        file(MAKE_DIRECTORY "${LIB_INSTALL_DIR}")
    endif()
    
    # Check if library needs to be downloaded
    set(NEEDS_DOWNLOAD TRUE)
    if(EXISTS "${LIB_VERSION_FILE}")
        file(READ "${LIB_VERSION_FILE}" INSTALLED_VERSION)
        string(STRIP "${INSTALLED_VERSION}" INSTALLED_VERSION)
        if("${INSTALLED_VERSION}" STREQUAL "${LIB_VERSION}")
            set(NEEDS_DOWNLOAD FALSE)
            message(STATUS "${LIB_NAME} ${INSTALLED_VERSION} already installed, skipping download")
        else()
            message(STATUS "${LIB_NAME} version mismatch (installed: ${INSTALLED_VERSION}, requested: ${LIB_VERSION})")
        endif()
    endif()
    
    if(NEEDS_DOWNLOAD)
        message(STATUS "Downloading ${LIB_NAME} ${LIB_VERSION}...")
        
        # Create external directory if it doesn't exist
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/external")
        
        set(LIB_ZIP "${CMAKE_SOURCE_DIR}/external/${LIB_NAME}-${LIB_VERSION}.zip")
        
        # Download library binaries
        file(DOWNLOAD "${DOWNLOAD_URL}" "${LIB_ZIP}" 
            SHOW_PROGRESS
            STATUS DOWNLOAD_STATUS)
        
        list(GET DOWNLOAD_STATUS 0 DOWNLOAD_RESULT)
        if(NOT DOWNLOAD_RESULT EQUAL 0)
            list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)
            message(FATAL_ERROR "Failed to download ${LIB_NAME}: ${ERROR_MESSAGE}")
        endif()
        
        # Extract the zip file
        message(STATUS "Extracting ${LIB_NAME}...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf "${LIB_ZIP}" --format=zip
            WORKING_DIRECTORY "${LIB_INSTALL_DIR}/"
            RESULT_VARIABLE EXTRACT_RESULT
        )
        
        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract ${LIB_NAME} zip file")
        endif()
        
        # Delete the zip file
        file(REMOVE "${LIB_ZIP}")
        
        # Create version file to track installed version
        file(WRITE "${LIB_VERSION_FILE}" "${LIB_VERSION}")
        message(STATUS "${LIB_NAME} ${LIB_VERSION} installed successfully")
    endif()
endfunction()

file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

set(DXC_VERSION "v1.9.2602")
set(DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_VERSION}/dxc_2026_02_20.zip")
download_library("dxc" "${DXC_VERSION}" "${DXC_URL}" "dxc_*")
set(DXC_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/dxc")
file(CREATE_LINK "${DXC_INSTALL_DIR}/bin/x64/dxc.exe" "${CMAKE_SOURCE_DIR}/bin/dxc.exe" SYMBOLIC)
file(CREATE_LINK "${DXC_INSTALL_DIR}/bin/x64/dxcompiler.dll" "${CMAKE_SOURCE_DIR}/bin/dxcompiler.dll" SYMBOLIC)
file(CREATE_LINK "${DXC_INSTALL_DIR}/bin/x64/dxil.dll" "${CMAKE_SOURCE_DIR}/bin/dxil.dll" SYMBOLIC)

file(GLOB_RECURSE SOURCES 
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp"
)

# Exclude hlsl_runtime_test.cpp from main srrhi executable (it's a separate target)
list(FILTER SOURCES EXCLUDE REGEX "hlsl_runtime_test\\.cpp$")

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE "${DXC_INSTALL_DIR}/inc")
target_link_libraries(${PROJECT_NAME} PRIVATE "${DXC_INSTALL_DIR}/lib/x64/dxcompiler.lib" "${DXC_INSTALL_DIR}/lib/x64/dxil.lib")

# ============================================================================
# Validation executables: each generated header gets its own validation binary
# Checks for C++ compilation errors and validates struct layouts per header
# 
# Usage:
#   1. Run: cd bin && .\srrhi.exe -i ..\test\input -o ..\test\output --test
#   2. Run: cd .. && python generate_validation.py  
#   3. Build target: ALL_BUILD (builds all validation targets)
#   4. Run any validation executable: .\bin\validation_test_arrays.exe, etc.
# ============================================================================

# Find all validation_*.cpp files from both auto-generated and manual folders
file(GLOB VALIDATION_SOURCES 
    "${CMAKE_SOURCE_DIR}/test/output/cpp/validation_*.cpp"
    "${CMAKE_SOURCE_DIR}/test/manual_validation/validation_*.cpp"
)

# Create an executable for each validation file
foreach(VALIDATION_FILE ${VALIDATION_SOURCES})
    get_filename_component(FILENAME ${VALIDATION_FILE} NAME)
    string(REPLACE ".cpp" "" TARGET_NAME ${FILENAME})
    
    add_executable(${TARGET_NAME} ${VALIDATION_FILE})
    
    target_include_directories(${TARGET_NAME} 
        PRIVATE "${CMAKE_SOURCE_DIR}/test/output/cpp"
        PRIVATE "${CMAKE_SOURCE_DIR}/test/manual_validation"
        PRIVATE "${CMAKE_SOURCE_DIR}"
        PRIVATE "${DXC_INSTALL_DIR}/inc"
    )
    
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
    
    message(STATUS "Created validation target: ${TARGET_NAME}")
endforeach()

# ============================================================================
# HLSL runtime validation executable
# Uses D3D12 (WARP software adapter) + DXC to compile and execute compute
# shaders built from generated .hlsli files, then verifies the UAV output
# matches expected bit-exact values.
#
# Usage:
#   cd bin
#   .\hlsl_runtime_validation.exe
# ============================================================================

add_executable(hlsl_runtime_validation
    "${CMAKE_SOURCE_DIR}/src/hlsl_runtime_test.cpp"
)

target_include_directories(hlsl_runtime_validation
    PRIVATE "${DXC_INSTALL_DIR}/inc"
)

target_link_libraries(hlsl_runtime_validation
    PRIVATE "${DXC_INSTALL_DIR}/lib/x64/dxcompiler.lib"
    PRIVATE d3d12
    PRIVATE dxgi
)

set_target_properties(hlsl_runtime_validation PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

message(STATUS "Created HLSL runtime validation target: hlsl_runtime_validation")
