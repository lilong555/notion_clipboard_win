if(NOT DEFINED INPUT OR INPUT STREQUAL "")
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Notion fixture output does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" CONTENT)
string(LENGTH "${CONTENT}" CONTENT_LENGTH)
if(CONTENT_LENGTH LESS 10000)
    message(FATAL_ERROR "Notion fixture output is unexpectedly short: ${CONTENT_LENGTH} bytes")
endif()

string(JSON BLOCK_COUNT ERROR_VARIABLE JSON_ERROR LENGTH "${CONTENT}")
if(JSON_ERROR)
    message(FATAL_ERROR "Notion fixture output is not valid JSON: ${JSON_ERROR}")
endif()
if(NOT BLOCK_COUNT EQUAL 160)
    message(FATAL_ERROR "Notion fixture block count changed: expected 160, got ${BLOCK_COUNT}")
endif()

string(FIND "${CONTENT}" "\"type\":\"heading_1\"" HEADING_POS)
if(HEADING_POS EQUAL -1)
    message(FATAL_ERROR "Notion fixture output is missing heading blocks")
endif()

string(REGEX MATCHALL "\"type\":\"equation\"" EQUATION_MATCHES "${CONTENT}")
list(LENGTH EQUATION_MATCHES EQUATION_COUNT)
if(NOT EQUATION_COUNT EQUAL 115)
    message(FATAL_ERROR "Notion fixture equation count changed: expected 115, got ${EQUATION_COUNT}")
endif()

foreach(EXPECTED IN ITEMS "AAABBCAA" "\"expression\":\"A\"" "AAA" "AA" "BB")
    string(FIND "${CONTENT}" "${EXPECTED}" EXPECTED_POS)
    if(EXPECTED_POS EQUAL -1)
        message(FATAL_ERROR "Notion fixture output is missing expected content: ${EXPECTED}")
    endif()
endforeach()

foreach(FORBIDDEN IN ITEMS
        "$AAA$"
        "$AA$"
        "$BB$"
        "\"expression\":\"AAA\""
        "\"expression\":\"AA\""
        "\"expression\":\"BB\""
        "\"link\":{\"url\":")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FORBIDDEN_POS)
    if(NOT FORBIDDEN_POS EQUAL -1)
        message(FATAL_ERROR "Notion fixture output contains forbidden conversion artifact: ${FORBIDDEN}")
    endif()
endforeach()

message(STATUS "Verified Notion fixture output: ${CONTENT_LENGTH} bytes, ${BLOCK_COUNT} blocks")
