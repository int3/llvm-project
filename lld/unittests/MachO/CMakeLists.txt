add_lld_unittests(LLDMachOTests
  CodeSignatureFlushTest.cpp
  RelocationsTest.cpp
  RelocationStorageTest.cpp
)

target_include_directories(LLDMachOTests PRIVATE
  ${LLVM_MAIN_SRC_DIR}/../libunwind/include
)

target_link_libraries(LLDMachOTests
  PRIVATE
  lldCommon
  lldMachO
)
