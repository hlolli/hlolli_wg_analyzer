if(NOT DEFINED HWA_WASM_MODULE OR NOT DEFINED HWA_WASM_TOOLS)
  message(FATAL_ERROR "HWA_WASM_MODULE and HWA_WASM_TOOLS are required")
endif()

execute_process(
  COMMAND "${HWA_WASM_TOOLS}" print "${HWA_WASM_MODULE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE wat
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "wasm-tools failed: ${stderr}")
endif()
if(wat MATCHES "\\(import [^\n]*\"(path_|sock_|proc_|fd_prestat_)")
  message(FATAL_ERROR "portable WASM module has a path, socket, or process import")
endif()
