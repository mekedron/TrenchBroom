add_library(TbMcpTestUtilsLib STATIC)

target_sources(TbMcpTestUtilsLib
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FakeHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FakeScheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpToolFixture.cpp

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/FakeHost.h
      ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/FakeScheduler.h
      ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/McpToolFixture.h
)

target_link_libraries(TbMcpTestUtilsLib
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
  PUBLIC
    Catch2::Catch2
    TbMcpLib
    TbAppTestUtilsLib
    TbMdlTestUtilsLib
    TbFsTestUtilsLib
    TbBaseTestUtilsLib
)
