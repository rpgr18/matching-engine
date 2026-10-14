# Strict warnings for first-party targets. Third-party code (GoogleTest) is
# pulled in as SYSTEM so none of this applies to it.
add_library(lob_warnings INTERFACE)

if(MSVC)
  target_compile_options(lob_warnings INTERFACE /W4 /permissive- $<$<BOOL:${LOB_WERROR}>:/WX>)
else()
  target_compile_options(lob_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wimplicit-fallthrough
    $<$<BOOL:${LOB_WERROR}>:-Werror>)
endif()
