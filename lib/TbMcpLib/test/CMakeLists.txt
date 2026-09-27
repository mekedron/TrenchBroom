add_executable(TbMcpLibTest)
EMBED_UTF8_MANIFEST(TbMcpLibTest)

target_sources(TbMcpLibTest
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunAllTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ActionTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_AgentCamera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Annotations.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Args.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_BridgeSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_BrushEditTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CallLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CallRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CameraProjection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ChangeCollector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ClipboardTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompileLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompileTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompileUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ConsoleBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ConsoleTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CsgUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DocumentTargeting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DocumentTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EngineTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityClassTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityCreateTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityModelTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityModelUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityPropertyTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FaceTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GameTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GeometryTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GeometryUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GroupTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_HistoryTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_HttpParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_HttpResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Image.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_JsonRpc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_JsonVm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ListDetail.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_KnowledgeTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LayerTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ManifestTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapCheckTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapManifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialKnowledge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialKnowledgeTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ObjectIds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Pagination.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PreferenceTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PromptRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Prompts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PickTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PlacementChecks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ResourceRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Scenarios.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SceneTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Schema.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SpaceAnalysis.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SpaceTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SelectionTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SessionTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SnapshotTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SpatialTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SseParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_StreamableHttp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TagTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Targets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ToolCatalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ToolRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TransformTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpstreamCommandProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpstreamLoadAssimpModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpstreamMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpstreamNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpstreamQuickFixes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UvCheck.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UvTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UvWarnings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ValidationTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ViewTools.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

# The preference catalog test checks the catalog against the editor's preference header
target_compile_definitions(TbMcpLibTest PRIVATE
  MCP_TEST_PREFERENCES_HEADER="${CMAKE_SOURCE_DIR}/lib/TbPreferencesLib/include/prefs/Preferences.h")

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
