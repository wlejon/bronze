bronze_add_module(abi
    SOURCES
        abi_check.c
)

# The ABI fingerprint: the first 32 bits of bronze_abi.h's own SHA-256,
# computed at configure and recomputed whenever the header changes (the
# CONFIGURE_DEPENDS below is what makes an edit re-run configure). The
# header IS the ABI — every helper signature, data symbol, layout constant
# and bit pattern generated code relies on lives in it — so its content
# hash names the ABI version without a number anyone can forget to bump.
#
# codegen-llvm stamps the value into every emitted object as
# `bronze_object_abi_fingerprint`; the runtime compares the object's stamp
# against its own value at program entry (runtime/abi_guard.h). PUBLIC so
# both sides read one value from one place. A meaning-only ABI change that
# would otherwise touch no header text must still edit the header — the
# comment naming the new meaning — which is the documentation discipline
# this file already demands.
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/bronze_abi.h" _bronze_abi_sha256)
string(SUBSTRING "${_bronze_abi_sha256}" 0 8 _bronze_abi_fingerprint)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS bronze_abi.h)
target_compile_definitions(bronze_abi PUBLIC "BRONZE_ABI_FINGERPRINT=0x${_bronze_abi_fingerprint}u")
