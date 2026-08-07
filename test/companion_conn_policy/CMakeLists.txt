add_executable(CompanionConnPolicyTest
  CompanionConnPolicyTest.cpp
  ${REPO_ROOT}/src/CompanionConnPolicy.cpp
)

target_include_directories(CompanionConnPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionConnPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionConnPolicyTest)
