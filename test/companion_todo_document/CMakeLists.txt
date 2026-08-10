add_executable(CompanionTodoDocumentTest
  CompanionTodoDocumentTest.cpp
  ${REPO_ROOT}/src/CompanionTodoDocument.cpp
)

target_include_directories(CompanionTodoDocumentTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionTodoDocumentTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionTodoDocumentTest)
