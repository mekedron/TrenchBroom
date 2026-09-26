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
)
