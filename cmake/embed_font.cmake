if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
  message(FATAL_ERROR "embed_font.cmake requires INPUT, OUTPUT, and SYMBOL")
endif()

file(READ "${INPUT}" HEX_DATA HEX)
string(LENGTH "${HEX_DATA}" HEX_LEN)
math(EXPR BYTE_LEN "${HEX_LEN} / 2")

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," C_BYTES "${HEX_DATA}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n" C_BYTES "${C_BYTES}")

file(WRITE "${OUTPUT}"
  "#include <stddef.h>\n"
  "const unsigned char ${SYMBOL}[] = {\n${C_BYTES}\n};\n"
  "const size_t ${SYMBOL}_size = ${BYTE_LEN};\n"
)
