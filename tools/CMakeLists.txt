add_executable(lob_replay lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob::lob lob_warnings)

add_executable(lob_shell lob_shell.cpp)
target_link_libraries(lob_shell PRIVATE lob::lob lob_warnings)
