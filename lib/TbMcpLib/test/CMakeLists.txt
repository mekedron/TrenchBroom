add_executable(TbMcpLibTest)
EMBED_UTF8_MANIFEST(TbMcpLibTest)

target_sources(TbMcpLibTest
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunAllTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Args.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_BrushEditTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CallLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CallRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ChangeCollector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DocumentTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityClassTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityCreateTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityPropertyTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FaceTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GameTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GeometryTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_HistoryTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_HttpParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_HttpResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_JsonRpc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_JsonVm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ObjectIds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Pagination.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PromptRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ResourceRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Scenarios.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SceneTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Schema.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SelectionTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SessionTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SpatialTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SseParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_StreamableHttp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TagTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Targets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ToolRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TransformTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ViewTools.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_link_libraries(TbMcpLibTest
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    Catch2::Catch2WithMain
    TbMcpLib
    TbMcpTestUtilsLib
    TbAppTestUtilsLib
    TbBaseTestUtilsLib
    TbFsTestUtilsLib
    TbMdlTestUtilsLib
)

# Copy fixtures
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbMcpLibTest>/fixture)

add_custom_command(TARGET TbMcpLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games-testing" "${TEST_FIXTURE_DEST_DIR}/games"
)

include(Catch)
catch_discover_tests(TbMcpLibTest)
