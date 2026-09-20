add_executable(HnJsonTest
  HnJsonTest.cpp
  ${REPO_ROOT}/src/network/HnJson.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(HnJsonTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/src/network
)

target_link_libraries(HnJsonTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HnJsonTest)
