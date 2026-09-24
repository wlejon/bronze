#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <thread>
#include "embed/embed.h"
#include "runtime/heap.h"
#include "runtime/value.h"

using namespace bronze;

TEST_CASE("HandleScope manages local handle roots on C++ stack") {
    embed::HandleScope outer;
    CHECK(outer.numberOfHandles() == 0);

    embed::Local<Value> num = outer.createLocal(embed::fromDouble(42.0));
    embed::Local<Value> str = outer.createLocal(embed::fromUtf8("hello"));

    CHECK(outer.numberOfHandles() == 2);
    CHECK(embed::toDouble(num.get()) == 42.0);
    CHECK(embed::toUtf8(str.get()) == "hello");

    {
        embed::HandleScope inner;
        embed::Local<Value> innerVal = inner.createLocal(embed::fromUtf8("nested"));
        CHECK(inner.numberOfHandles() == 1);
        CHECK(embed::toUtf8(innerVal.get()) == "nested");
    }

    CHECK(outer.numberOfHandles() == 2);
    CHECK(embed::toDouble(num.get()) == 42.0);
    CHECK(embed::toUtf8(str.get()) == "hello");
}

TEST_CASE("Local handles survive moving GC inside HandleScope") {
    embed::HandleScope scope;

    embed::Local<Value> str = scope.createLocal(embed::fromUtf8("survive_gc"));
    embed::Local<Value> obj = scope.createLocal(embed::createObject());
    embed::Local<Value> val = scope.createLocal(embed::fromUtf8("val"));
    obj.set(embed::setProperty(obj.get(), "key", val.get()));

    // Force garbage collections to trigger moving semispace relocations
    for (int i = 0; i < 3; ++i) {
        embed::collectGarbage();
    }

    CHECK(embed::toUtf8(str.get()) == "survive_gc");
    embed::Value readBack = embed::getProperty(obj.get(), "key");
    CHECK(embed::toUtf8(readBack) == "val");
}

TEST_CASE("EscapableHandleScope promotes handle to parent scope") {
    embed::HandleScope outer;
    CHECK(outer.numberOfHandles() == 0);

    embed::Local<Value> escaped;
    {
        embed::EscapableHandleScope inner;
        embed::Local<Value> temp1 = inner.createLocal(embed::fromDouble(100.0));
        embed::Local<Value> target = inner.createLocal(embed::fromUtf8("escaped_str"));
        embed::Local<Value> temp2 = inner.createLocal(embed::fromDouble(200.0));
        (void)temp1;
        (void)temp2;

        escaped = inner.escape(target);
    }

    // After inner scope exits, outer has 1 handle (the escaped one)
    CHECK(outer.numberOfHandles() == 1);

    // Verify it survives garbage collection
    embed::collectGarbage();
    CHECK(embed::toUtf8(escaped.get()) == "escaped_str");
}

TEST_CASE("Local handle copy, move, and mutation semantics") {
    embed::HandleScope scope;

    embed::Local<Value> original = scope.createLocal(embed::fromDouble(1.0));
    embed::Local<Value> copy = original;
    CHECK(embed::toDouble(copy.get()) == 1.0);

    copy.set(embed::fromDouble(2.0));
    CHECK(embed::toDouble(original.get()) == 2.0);
    CHECK(embed::toDouble(copy.get()) == 2.0);

    embed::Local<Value> moved = std::move(copy);
    CHECK(copy.isEmpty());
    CHECK(!moved.isEmpty());
    CHECK(embed::toDouble(moved.get()) == 2.0);
}

// A Persistent that outlives its thread's slot registry: the shape of a host
// keeping a callback in a static, whose destructor runs after exit() has torn
// down the main thread's thread_locals. The holder is constructed before the
// registry (the Persistent inside it is what first touches the registry), so
// thread exit destroys the registry first and the holder's release comes
// after. Under ASan or a debug heap, releasing into the dead registry is a
// use-after-free.
TEST_CASE("Persistent released after its thread's registry is torn down") {
    bool readBack = false;
    std::thread worker([&readBack] {
        thread_local std::optional<embed::Persistent> holder;
        holder.emplace(embed::fromUtf8("outlives the registry"));
        readBack = embed::toUtf8(holder->get()) == "outlives the registry";
    });
    worker.join();
    CHECK(readBack);
}
