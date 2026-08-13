add_executable(CompanionButtonPolicyTest
  CompanionButtonPolicyTest.cpp
  ${REPO_ROOT}/src/CompanionButtonPolicy.cpp
)

target_include_directories(CompanionButtonPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionButtonPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionButtonPolicyTest)
