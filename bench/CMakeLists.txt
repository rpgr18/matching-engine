find_package(Threads REQUIRED)

add_executable(lob_bench bench_book.cpp)
target_link_libraries(lob_bench PRIVATE lob::lob lob_warnings)

add_executable(lob_pipeline bench_pipeline.cpp)
target_link_libraries(lob_pipeline PRIVATE lob::lob lob_warnings Threads::Threads)
