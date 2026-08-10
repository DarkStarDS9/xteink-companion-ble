add_executable(CompanionListJsonWriterTest
  CompanionListJsonWriterTest.cpp
  ${REPO_ROOT}/src/CompanionListJsonWriter.cpp
  ${REPO_ROOT}/src/CompanionTodoDocument.cpp
)

target_include_directories(CompanionListJsonWriterTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionListJsonWriterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionListJsonWriterTest)
