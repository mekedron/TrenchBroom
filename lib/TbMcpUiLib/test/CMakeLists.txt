add_executable(TbMcpUiLibTest)
EMBED_UTF8_MANIFEST(TbMcpUiLibTest)

target_sources(TbMcpUiLibTest
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpUiTestUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpUiTestUtils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunAllTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpAnnotations.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpCompile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpCompileHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpConsoleHook.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpServerController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpSnapshotRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpTcpTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpUiIntegration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QtMcpHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpstreamHooks.cpp

  PUBLIC FILE_SET cmd_tool TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

# The snapshot renderer tests load shaders and fonts from the editor's resources and
# render the MCP sample maps
target_compile_definitions(TbMcpUiLibTest PRIVATE
  MCP_TEST_RESOURCE_DIR="${TB_RESOURCE_DIR}"
  MCP_TEST_MAPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../TbMcpLib/test/fixture/mcp/maps")

target_link_libraries(TbMcpUiLibTest PRIVATE
  CompilerConfig
  PrecompileQtHeaders
  PrecompileStdHeaders
  Catch2::Catch2WithMain
  Qt6::Network
  Qt6::Test
  Qt6::Widgets
  TbAppTestUtilsLib
  TbBaseTestUtilsLib
  TbFsTestUtilsLib
  TbMcpTestUtilsLib
  TbMcpUiLib
  TbMdlTestUtilsLib
  TbUiTestUtilsLib
)

set_target_properties(TbMcpUiLibTest PROPERTIES AUTOMOC TRUE)

add_dependencies(TbMcpUiLibTest CmdTool)
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/CmdTool.h"
  CONTENT "#define CMD_TOOL_PATH \"$<TARGET_FILE:CmdTool>\"")

# Copy fixtures; the tests use the game fixtures of TbUiLibTest
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../TbUiLib/test/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbMcpUiLibTest>/fixture)

add_custom_command(TARGET TbMcpUiLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games-testing" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/graphics/images" "$<TARGET_FILE_DIR:TbMcpUiLibTest>/images"
)

include(Catch)
catch_discover_tests(TbMcpUiLibTest)
