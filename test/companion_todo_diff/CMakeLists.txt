add_executable(CompanionTodoDiffTest
  CompanionTodoDiffTest.cpp
  ${REPO_ROOT}/src/CompanionTodoDiff.cpp
)

target_include_directories(CompanionTodoDiffTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionTodoDiffTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionTodoDiffTest)
