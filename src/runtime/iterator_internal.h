#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

StringHeader* keyNext();
StringHeader* keyDone();
StringHeader* keyValue();
StringHeader* keyReturn();

bool isCallable(Value v);
Value callMethod(Rooted<Value>& fn, Rooted<Value>& thisValue);
Value namedProp(Value obj, StringHeader* key);
Value proxyMethodOf(Value proxy, Value symbolKey);
uint64_t iteratorProtoSelf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*);

}  // namespace bronze::runtime
