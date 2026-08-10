add_executable(CompanionTodoNavTest
  CompanionTodoNavTest.cpp
  ${REPO_ROOT}/src/CompanionTodoNav.cpp
)

target_include_directories(CompanionTodoNavTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionTodoNavTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionTodoNavTest)
