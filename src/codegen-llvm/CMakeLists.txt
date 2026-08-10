# LLVM backend — lands in foundation phase 2 (docs/0001-foundation.md).
# The gate exists now so the build graph and provisioning workflow are
# settled before the heavy dependency arrives. Enabling the option before
# the backend exists is a loud configure error, not a stub.
message(FATAL_ERROR
    "BRONZE_WITH_LLVM=ON but src/codegen-llvm is not implemented yet. "
    "Provision LLVM first (vcpkg install --x-feature=llvm, one-time build), "
    "then implement Backend::emitObject here. See docs/0001-foundation.md phase 2.")
