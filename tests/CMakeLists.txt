# A pinned GoogleTest is fetched by default so every machine and CI runner tests
# against the same version. Set LOB_USE_SYSTEM_GTEST=ON to use an installed one.
option(LOB_USE_SYSTEM_GTEST "Use an installed GoogleTest instead of fetching one" OFF)

if(LOB_USE_SYSTEM_GTEST)
  find_package(GTest 1.11 REQUIRED)
else()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(lob_tests
  test_differential.cpp
  test_flat_order_map.cpp
  test_hierarchical_bitset.cpp
  test_ladders.cpp
  test_order_book.cpp
  test_order_pool.cpp
  test_self_trade.cpp
  test_types.cpp
)
target_link_libraries(lob_tests PRIVATE lob::lob lob_warnings GTest::gtest_main)
gtest_discover_tests(lob_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 300)
