set(LLVM_LINK_COMPONENTS
  Support
  TargetParser
  )

# Replacing the global operator new is a decision for the standalone lld
# executable, not for every tool in the multicall binary, so it is left out of
# the llvm-driver build.
if(LLVM_TOOL_LLVM_DRIVER_BUILD)
  set(LLD_OPERATOR_NEW_SOURCES "")
else()
  set(LLD_OPERATOR_NEW_SOURCES PoolOperatorNew.cpp)
endif()

add_lld_tool(lld
  lld.cpp
  ${LLD_OPERATOR_NEW_SOURCES}

  SUPPORT_PLUGINS
  GENERATE_DRIVER
  )
export_executable_symbols_for_plugins(lld)

function(lld_target_link_libraries target type)
  if (TARGET obj.${target})
    target_link_libraries(obj.${target} ${type} ${ARGN})
  endif()

  get_property(LLVM_DRIVER_TOOLS GLOBAL PROPERTY LLVM_DRIVER_TOOLS)
  if(LLVM_TOOL_LLVM_DRIVER_BUILD AND ${target} IN_LIST LLVM_DRIVER_TOOLS)
    set(target llvm-driver)
  endif()

  target_link_libraries(${target} ${type} ${ARGN})
endfunction()

lld_target_link_libraries(lld
  PRIVATE
  lldCommon
  lldCOFF
  lldELF
  lldMachO
  lldMinGW
  lldWasm
  )

if(NOT LLD_SYMLINKS_TO_CREATE)
  set(LLD_SYMLINKS_TO_CREATE
      lld-link ld.lld ld64.lld wasm-ld)
endif()

foreach(link ${LLD_SYMLINKS_TO_CREATE})
  add_lld_symlink(${link} lld)
endforeach()

if(LLVM_TOOL_LLVM_DRIVER_BUILD)
  set_property(GLOBAL APPEND PROPERTY LLVM_DRIVER_HIDDEN_TOOL_ALIASES_lld ld)
endif()
