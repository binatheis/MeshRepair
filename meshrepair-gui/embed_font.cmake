if(NOT DEFINED MESHREPAIR_FONT_TOOL OR NOT DEFINED MESHREPAIR_FONT_INPUT OR NOT DEFINED MESHREPAIR_FONT_SYMBOL
   OR NOT DEFINED MESHREPAIR_FONT_OUTPUT)
    message(FATAL_ERROR "Missing font embedding arguments")
endif()

execute_process(
    COMMAND "${MESHREPAIR_FONT_TOOL}" -u8 "${MESHREPAIR_FONT_INPUT}" "${MESHREPAIR_FONT_SYMBOL}"
    OUTPUT_FILE "${MESHREPAIR_FONT_OUTPUT}"
    RESULT_VARIABLE _meshrepair_font_result
    ERROR_VARIABLE _meshrepair_font_error
)

if(NOT _meshrepair_font_result EQUAL 0)
    message(FATAL_ERROR "Failed to embed font: ${_meshrepair_font_error}")
endif()
