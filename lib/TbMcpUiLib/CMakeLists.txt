add_library(TbMcpUiLib STATIC)

target_sources(TbMcpUiLib
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpCompileHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpConsoleHook.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpPreferencePane.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServerController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpSnapshotRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpStatusIndicator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpTcpTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpUiIntegration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/QtMcpHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/QtScheduler.cpp

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpCompileHost.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpConsoleHook.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpPreferencePane.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpPreferences.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpServerController.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpSnapshotRenderer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpStatusIndicator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpTcpTransport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/McpUiIntegration.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/QtMcpHost.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/QtScheduler.h
)

target_link_libraries(TbMcpUiLib
  PRIVATE
    CompilerConfig
    PrecompileQtHeaders
    PrecompileStdHeaders
    fmt::fmt-header-only
  PUBLIC
    KdLib
    Qt6::Network
    Qt6::OpenGL
    Qt6::Widgets
    TbAppLib
    TbBaseLib
    TbMcpLib
    TbMdlLib
    TbPreferencesLib
    TbUiLib
)

set_target_properties(TbMcpUiLib PROPERTIES AUTOMOC TRUE)

add_subdirectory(test)
