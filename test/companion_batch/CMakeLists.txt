add_executable(CompanionBatchModelTest
  CompanionBatchModelTest.cpp
  ${REPO_ROOT}/src/CompanionBatchModel.cpp
)

target_include_directories(CompanionBatchModelTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionBatchModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionBatchModelTest)
