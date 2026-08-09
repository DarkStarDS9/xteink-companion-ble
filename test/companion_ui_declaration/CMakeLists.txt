add_executable(CompanionUiDeclarationTest
  CompanionUiDeclarationTest.cpp
  ${REPO_ROOT}/src/CompanionUiDeclaration.cpp
)

target_include_directories(CompanionUiDeclarationTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CompanionUiDeclarationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionUiDeclarationTest)
