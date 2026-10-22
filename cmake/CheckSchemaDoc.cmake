# Fails when docs/spec/protocol-params.md differs from what dc-cli generates from the schema.
# Usage: cmake -DDC_CLI=<path> -DDOC=<path> -P CheckSchemaDoc.cmake
execute_process(COMMAND "${DC_CLI}" schema --markdown OUTPUT_VARIABLE generated RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "dc-cli schema failed: ${rc}")
endif()
file(READ "${DOC}" doc)
string(REPLACE "\r\n" "\n" doc "${doc}")
string(REPLACE "\r\n" "\n" generated "${generated}")
string(FIND "${doc}" "${generated}" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "docs/spec/protocol-params.md is out of date. Regenerate it with:\n"
                        "  dc-cli schema --markdown  (see the header of the file)")
endif()
