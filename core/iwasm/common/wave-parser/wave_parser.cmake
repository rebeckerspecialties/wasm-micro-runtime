# Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

set(WAVE_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wave_parser)
file(MAKE_DIRECTORY ${WAVE_GEN_DIR})

set(BISON_HDR ${WAVE_GEN_DIR}/wave_parser.h)
set(BISON_SRC ${WAVE_GEN_DIR}/wave_parser.c)
set(FLEX_SRC  ${WAVE_GEN_DIR}/wave_lexer.c)
set(FLEX_HDR  ${WAVE_GEN_DIR}/wave_lexer.h)

# --report=all and --feature=caret are diagnostic-only (a .output report and
# caret-style error display) and require Bison >= 2.6. macOS/Xcode ships GNU
# Bison 2.3, which aborts on --feature=caret, so only pass these when the host
# Bison supports them; the generated parser is byte-for-byte identical either way.
set(WAVE_BISON_COMPILE_FLAGS "")
if (BISON_VERSION AND NOT BISON_VERSION VERSION_LESS "2.6")
    set(WAVE_BISON_COMPILE_FLAGS "--report=all --feature=caret")
endif ()

BISON_TARGET(WaveParser
    ${CMAKE_CURRENT_LIST_DIR}/wave_parser.y
    ${BISON_SRC}
    DEFINES_FILE ${BISON_HDR}
    COMPILE_FLAGS "${WAVE_BISON_COMPILE_FLAGS}"
)

FLEX_TARGET(WaveScanner
    ${CMAKE_CURRENT_LIST_DIR}/wave_lexer.l
    ${FLEX_SRC}
    COMPILE_FLAGS "--header-file=${FLEX_HDR}"
)

ADD_FLEX_BISON_DEPENDENCY(WaveScanner WaveParser)


set(WAVE_PARSER_SOURCES
    ${BISON_WaveParser_OUTPUTS}
    ${FLEX_WaveScanner_OUTPUTS}
    ${CMAKE_CURRENT_LIST_DIR}/wave_adapter.c
)

set(WAVE_PARSER_INCLUDE_DIRS 
    ${WAVE_GEN_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
)

message(STATUS "Wave Parser sources: ${WAVE_PARSER_SOURCES}")