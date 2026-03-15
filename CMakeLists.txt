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

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE "${DXC_INSTALL_DIR}/inc")
target_link_libraries(${PROJECT_NAME} PRIVATE "${DXC_INSTALL_DIR}/lib/x64/dxcompiler.lib" "${DXC_INSTALL_DIR}/lib/x64/dxil.lib")
