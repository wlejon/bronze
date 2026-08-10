/* Compiled as C on purpose: proves bronze_abi.h stays expressible in pure
 * C. If a C++ type ever leaks into the ABI registry, this file — not a
 * segfault three layers into a dynamic call — is what breaks. */
#include "abi/bronze_abi.h"
