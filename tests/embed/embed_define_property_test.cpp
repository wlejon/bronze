// embed.h defineProperty: the attributes a host spells. A WebIDL constant is
// read-only, enumerable and permanent, on a plain object and on a function
// (the interface object's statics) alike.

#include <doctest/doctest.h>

#include <span>

#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

using namespace bronze;

namespace {

void checkConstantOn(Value holder, const char* name, double value) {
    ShadowStackFrame frame;
    Rooted<Value> self{holder};
    if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        self.set(self.get().asObject<FunctionHeader>()->properties);
    }
    Rooted<Value> key{runtime::rtMakeString(name)};
    PropertyInfo info;
    auto* obj = self.get().asObject<ObjectHeader>();
    REQUIRE(obj->shape->lookupProperty(PropertyKey::fromValue(key.get()), info));
    CHECK(!info.accessor);
    CHECK(!info.writable);
    CHECK(info.enumerable);
    CHECK(!info.configurable);
    CHECK(obj->getSlot(info.slot).asNumber() == value);
    // An assignment is refused (NotWritable), and the value stays.
    Rooted<Value> other{Value::fromDouble(1.0)};
    SetRefusal refused = SetRefusal::None;
    obj->setProp(runtime::rtHeap(), runtime::rtArena(), key, other, nullptr, true, false, nullptr, &refused);
    CHECK(refused == SetRefusal::NotWritable);
    CHECK(self.get().asObject<ObjectHeader>()->getProp(runtime::rtHeap(), key).asNumber() == value);
}

}  // namespace

TEST_CASE("defineProperty spells a constant's attributes on objects and functions") {
    embed::Persistent obj{embed::createObject()};
    obj.set(embed::defineProperty(obj.get(), "FLOAT", embed::fromDouble(5126.0), embed::kConstantAttributes));
    obj.set(embed::setProperty(obj.get(), "plain", embed::fromDouble(1.0)));
    checkConstantOn(obj.get(), "FLOAT", 5126.0);
    CHECK(!embed::deleteProperty(obj.get(), "FLOAT"));
    checkConstantOn(obj.get(), "FLOAT", 5126.0);

    embed::Persistent fn{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value { return embed::undefined(); }, 0,
        "Iface")};
    fn.set(embed::defineProperty(fn.get(), "RGBA", embed::fromDouble(6408.0), embed::kConstantAttributes));
    checkConstantOn(fn.get(), "RGBA", 6408.0);
}
