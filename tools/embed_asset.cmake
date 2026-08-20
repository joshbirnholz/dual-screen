# Turns a binary asset into a C++ source file at configure time, so the app
# stays a single self-contained binary without a checked-in megabyte of hex.
#
# Usage: cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed_asset.cmake
file(READ "${INPUT}" HEX_DATA HEX)
string(LENGTH "${HEX_DATA}" HEX_LENGTH)
math(EXPR BYTE_COUNT "${HEX_LENGTH} / 2")

# One byte per "0x.." with a line break every 16, which keeps the generated file
# openable in an editor.
string(REGEX REPLACE "(..)" "0x\\1," BYTES "${HEX_DATA}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n    " BYTES "${BYTES}")

get_filename_component(INPUT_NAME "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
"// Generated from assets/${INPUT_NAME} by tools/embed_asset.cmake. Do not edit.
#include \"render/embedded_assets.hpp\"

namespace ds::assets {

extern const unsigned char ${SYMBOL}[] = {
    ${BYTES}
};
extern const unsigned long ${SYMBOL}_size = ${BYTE_COUNT}ul;

}  // namespace ds::assets
")
