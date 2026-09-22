bronze_add_module(abi
    SOURCES
        abi_check.c
)

# The ABI fingerprint: the first 32 bits of the SHA-256 of bronze_abi.h and
# bronze_abi_tls.h, computed at configure and recomputed whenever either header
# changes (the CONFIGURE_DEPENDS below is what makes an edit re-run configure).
# These headers ARE the ABI — every helper signature, data symbol, layout constant,
# thread-local block layout, and bit pattern generated code relies on lives in
# them — so their combined content hash names the ABI version without a number
# anyone can forget to bump. The backend stamps the value into every emitted object
# as `bronze_object_abi_fingerprint`; the runtime compares the object's stamp
# against its own value at program entry (runtime/abi_guard.h). PUBLIC so
# both sides read one value from one place.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/bronze_abi.h" _abi_content)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/bronze_abi_tls.h" _abi_tls_content)
string(SHA256 _bronze_abi_sha256 "${_abi_content}${_abi_tls_content}")
string(SUBSTRING "${_bronze_abi_sha256}" 0 8 _bronze_abi_fingerprint)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS bronze_abi.h bronze_abi_tls.h)
target_compile_definitions(bronze_abi PUBLIC "BRONZE_ABI_FINGERPRINT=0x${_bronze_abi_fingerprint}u")
