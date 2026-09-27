add_executable(TrenchBroomMcp)
EMBED_UTF8_MANIFEST(TrenchBroomMcp)

target_sources(TrenchBroomMcp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp
)

target_link_libraries(TrenchBroomMcp
  PRIVATE
    CompilerConfig
    Qt6::Core
    Qt6::Network
    TbMcpLib
    TbVersionLib
)

# The bridge is shipped next to the editor executable (inside the bundle on macOS, see
# app/TrenchBroom/CMakeLists.txt) so that it can find and launch the editor.
add_dependencies(TrenchBroom TrenchBroomMcp)
if(WIN32)
    install(TARGETS TrenchBroomMcp RUNTIME DESTINATION . COMPONENT TrenchBroom)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    install(TARGETS TrenchBroomMcp RUNTIME DESTINATION bin COMPONENT TrenchBroom)
endif()
