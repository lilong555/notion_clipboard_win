if(NOT DEFINED INPUT OR INPUT STREQUAL "")
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Obsidian fixture output does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" CONTENT)
string(LENGTH "${CONTENT}" CONTENT_LENGTH)
if(CONTENT_LENGTH LESS 1000)
    message(FATAL_ERROR "Obsidian fixture output is unexpectedly short: ${CONTENT_LENGTH} bytes")
endif()

string(FIND "${CONTENT}" "# " HEADING_POS)
if(NOT HEADING_POS EQUAL 0)
    message(FATAL_ERROR "Obsidian fixture output does not start with a Markdown heading")
endif()

string(FIND "${CONTENT}" "$A$" INLINE_MATH_POS)
if(INLINE_MATH_POS EQUAL -1)
    message(FATAL_ERROR "Obsidian fixture output is missing expected inline math")
endif()

string(FIND "${CONTENT}" "$$" DISPLAY_MATH_POS)
if(DISPLAY_MATH_POS EQUAL -1)
    message(FATAL_ERROR "Obsidian fixture output is missing expected display math")
endif()

string(FIND "${CONTENT}" "AAABBCAA" SAMPLE_POS)
if(SAMPLE_POS EQUAL -1)
    message(FATAL_ERROR "Obsidian fixture output is missing expected sample content")
endif()

string(FIND "${CONTENT}" "source: notion_clipboard_win" SOURCE_MARKER_POS)
if(NOT SOURCE_MARKER_POS EQUAL -1)
    message(FATAL_ERROR "Obsidian fixture output contains the removed source marker")
endif()

message(STATUS "Verified Obsidian fixture output: ${CONTENT_LENGTH} bytes")
