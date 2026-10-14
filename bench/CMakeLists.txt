add_executable(lob_bench bench_book.cpp)
target_link_libraries(lob_bench PRIVATE lob::lob lob_warnings)
