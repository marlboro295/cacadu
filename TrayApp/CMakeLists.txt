cmake_minimum_required(VERSION 3.16)
project(TrayApp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
if (MSVC)
    add_compile_options(/utf-8)
endif()

set(RPC_GENERATED_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/generated/rpc
)
set(MIDL_OUTPUTS
    ${RPC_GENERATED_DIR}/tray_c.c
    ${RPC_GENERATED_DIR}/tray_s.c
    ${RPC_GENERATED_DIR}/tray_h.h
)

add_custom_command(
    OUTPUT ${MIDL_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RPC_GENERATED_DIR}
    COMMAND midl /nologo /env x64 /out ${RPC_GENERATED_DIR}
            /header tray_h.h
            ${CMAKE_SOURCE_DIR}/idl/tray.idl
    DEPENDS ${CMAKE_SOURCE_DIR}/idl/tray.idl
    VERBATIM
)
add_custom_target(GenerateRpc DEPENDS ${MIDL_OUTPUTS})

add_executable(TrayApp WIN32
    src/main.cpp
    src/TrayIcon.cpp
    src/process_utils.cpp
    src/rpc_client.cpp
    src/service_control.cpp
    res/app.rc
    ${RPC_GENERATED_DIR}/tray_c.c
)
add_dependencies(TrayApp GenerateRpc)
target_include_directories(TrayApp PRIVATE src ${RPC_GENERATED_DIR})
target_link_libraries(TrayApp PRIVATE user32 shell32 gdi32 rpcrt4 advapi32)
set_target_properties(TrayApp PROPERTIES WIN32_EXECUTABLE TRUE VS_DPI_AWARE "PerMonitor")

add_executable(TrayService
    service/service.cpp
    service/session.cpp
    service/rpc.cpp
    service/rpc_security.cpp
    service/antivirus_manager.cpp
    service/http_client.cpp
    service/auth_manager.cpp
    service/license_manager.cpp
    ${RPC_GENERATED_DIR}/tray_s.c
)
add_dependencies(TrayService GenerateRpc)
target_compile_definitions(TrayService PRIVATE UNICODE _UNICODE)
if (MSVC)
    target_link_options(TrayService PRIVATE /ENTRY:wmainCRTStartup)
endif()
target_include_directories(TrayService PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${RPC_GENERATED_DIR}
)
target_link_libraries(TrayService PRIVATE
    user32 advapi32 rpcrt4 wtsapi32 userenv winhttp crypt32
)

install(TARGETS TrayApp TrayService
    RUNTIME DESTINATION .
)
