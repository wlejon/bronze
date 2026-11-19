#include "runtime/array.h"

#include <cstddef>
#include <cstdlib>
#include <utility>

#include "abi/bronze_abi.h"

#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"

namespace bronze {

namespace {

// Replace self's element block with one of `new_capacity` entries, copying
// the live prefix across. Allocation can collect and move both the array
// and its old block, so everything is re-derived through the root after
// the allocation.
// What generated code's inline array literal (llvm_construct.cpp) writes has
// to be exactly what `ArrayHeader::create` + `setCapacity` write.
// (array.h pins the field offsets and the header size.)
static_assert(HeapKind::ValueBlock == BRONZE_ABI_OBJ_FLAGS_VALUE_BLOCK);
static_assert(HeapKind::NumberBlock == BRONZE_ABI_OBJ_FLAGS_NUMBER_BLOCK);
static_assert(HeapKind::Array == BRONZE_ABI_OBJ_FLAGS_ARRAY);

void setCapacity(Heap& heap, Rooted<Value>& self, uint32_t new_capacity) {
    HeapObjectHeader* block = heap.allocate(new_capacity * sizeof(Value), Tag::Object);
    auto* arr = self.get().asObject<ArrayHeader>();
    // Not the zero `Heap::allocate` leaves behind, which reads as
    // `HeapKind::Plain` — and a plain object is a thing the collector reads a
    // `Shape*` out of. Elements are Values (or raw Numbers + Holes when
    // NUMBERS holds, marked NumberBlock so GC tracing skips them in O(1)).
    block->flags = arr->elementsAreNumbers() ? HeapKind::NumberBlock : HeapKind::ValueBlock;

    HeapValue* slots = block->payload<HeapValue>();
    uint32_t i = 0;
    // Read through the const accessor: copying the elements across changes
    // none of them, so NUMBERS stays what it was.
    const Value* old_slots = std::as_const(*arr).elementsData();
    // The block is new, so one bulk barrier over the copied run is all the
    // collector needs (gcCopyValues), and a hole is not a reference, so the
    // fill needs none; a per-slot barrier was most of a growth.
    if (old_slots) {
        i = arr->length < new_capacity ? arr->length : new_capacity;
        gcCopyValues(block, slots, old_slots, i);
    }
    gcFillValues(block, slots + i, Value::fromHole(), new_capacity - i);

    arr->elements = Value::fromObject(block);
    arr->capacity = new_capacity;
    arr->head_offset = 0;
}

}  // namespace

ArrayHeader* ArrayHeader::create(Heap& heap, uint32_t initial_capacity) {
    // The shape first: its first use builds the thread's array root, and it
    // is an arena node, so nothing about it moves.
    Shape* shape = runtime::rtArrayShape();
    size_t payload_bytes = sizeof(ArrayHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    raw_hdr->flags = HeapKind::Array;
    auto* arr = reinterpret_cast<ArrayHeader*>(raw_hdr);
    arr->shape = shape;
    arr->overflow = Value::fromUndefined();
    arr->length = 0;
    arr->capacity = 0;
    arr->head_offset = 0;
    arr->reserved = 0;
    arr->elements = Value::fromUndefined();

    if (initial_capacity > 0) {
        Rooted<Value> self(Value::fromObject(arr));
        setCapacity(heap, self, initial_capacity);
        arr = self.get().asObject<ArrayHeader>();
    }
    return arr;
}

void ArrayHeader::setLength(Heap& heap, Rooted<Value>& self, uint32_t newLength) {
    {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (newLength <= arr->length) {
            arr->truncate(newLength);
            return;
        }
        if (arr->head_offset + newLength > arr->capacity) {
            setCapacity(heap, self, newLength);
        }
    }
    // `setCapacity` allocates, so the header is re-derived through the root.
    ArrayHeader* arr = self.get().asObject<ArrayHeader>();
    HeapValue* slots = arr->elementsData();
    for (uint32_t i = arr->length; i < newLength; ++i) slots[i] = Value::fromHole();
    arr->length = newLength;
}

void ArrayHeader::appendRange(Heap& heap, Rooted<Value>& self, Rooted<Value>& src, uint32_t start,
                              uint32_t count, bool holesAsUndefined) {
    if (count == 0) return;
    const uint32_t at = self.get().asObject<ArrayHeader>()->length;
    const bool numbers =
        src.get().asObject<ArrayHeader>()->elementsAreNumbers() &&
        (at == 0 ? self.get().asObject<ArrayHeader>()->isBare()
                 : self.get().asObject<ArrayHeader>()->elementsAreNumbers());
    // Grows the block and leaves holes where the run goes; it allocates, so
    // every pointer is taken after it.
    setLength(heap, self, at + count);
    ArrayHeader* out = self.get().asObject<ArrayHeader>();
    const Value* from = std::as_const(*src.get().asObject<ArrayHeader>()).elementsData() + start;
    HeapValue* to = out->elementsData() + at;
    gcCopyValues(out->elementsBlock(), to, from, count);
    if (numbers) {
        // A run of Numbers after Numbers: no hole among them to rewrite.
        out->reserved |= kElementsAreNumbers;
        out->syncElementsKind();
    } else if (holesAsUndefined) {
        for (uint32_t i = 0; i < count; ++i) {
            if (HeapValue::values(to)[i].isHole()) to[i] = Value::fromUndefined();
        }
    }
}

void ArrayHeader::setElemSlow(Heap& heap, uint32_t index, Rooted<Value>& val) {
    Rooted<Value> self(Value::fromObject(this));
    if (head_offset + index >= capacity) {
        // Head room left by shift(): compact the elements down to index 0 when
        // that leaves at least half the block free, so the writes that fill it
        // pay for the copy. A fuller block grows instead — compacting one that
        // stays nearly full (a queue's push-one, shift-one) copied every
        // element on every push.
        if (head_offset > 0 && index + 1 <= capacity && length <= capacity / 2) {
            // Moving the elements down changes none of them, so NUMBERS
            // stays; the write below decides it.
            HeapValue* raw = elementsKeepingNumbers() - head_offset;
            const Value* cur = std::as_const(*this).elementsData();
            gcCopyValues(elementsBlock(), raw, cur, length);
            for (uint32_t i = length; i < capacity; ++i) {
                raw[i] = Value::fromHole();
            }
            head_offset = 0;
        } else {
            uint64_t new_cap = capacity ? static_cast<uint64_t>(capacity) + capacity / 2 + 16 : 4;
            while (new_cap <= index) {
                new_cap *= 2;
            }
            if (new_cap > 0xFFFFFFFFULL) {
                new_cap = static_cast<uint64_t>(index) + 1;
            }
            if (index >= 1000000000u ||
                static_cast<size_t>(new_cap) * sizeof(Value) + 64 >= Heap::kMaxObjectBytes) {
                runtime::rtThrowRangeError(
                    "Array allocation failed: index exceeds maximum heap capacity");
                return;
            }
            setCapacity(heap, self, static_cast<uint32_t>(new_cap));
        }
    }

    // `this` may be stale: setCapacity allocates.
    auto* arr = self.get().asObject<ArrayHeader>();
    if (index > arr->length) {
        // Holes below the new length: the mutable accessor forgets NUMBERS.
        for (uint32_t i = arr->length; i < index; ++i) {
            arr->elementsData()[i] = Value::fromHole();
        }
    }
    if (!val.get().isNumber()) arr->forgetNumbers();
    arr->elementsKeepingNumbers()[index] = val.get();
    if (index >= arr->length) {
        arr->length = index + 1;
    }
}

bool ArrayHeader::claimNumbers() noexcept {
    if (elementsAreNumbers()) return true;
    if ((reserved & kNumbersRefused) != 0) return false;
    // A refusal is remembered, so the readers that ask on every visit
    // (generic element reads, il_elem_lowering.cpp) scan an array once.
    auto refuse = [this] {
        reserved |= kNumbersRefused;
        return false;
    };
    if (!isBare() || length > BRONZE_ABI_ARRAY_CLAIM_MAX_LENGTH) return refuse();
    if (length > 0 && !elements.isPointer()) return refuse();
    const Value* cur = std::as_const(*this).elementsData();
    for (uint32_t i = 0; i < length; ++i) {
        const Value v = cur[i];
        if (v.isNumber()) continue;
        if (!v.isInt32()) return refuse();
    }
    // Every element is a Number; the Int32 boxes among them become the same
    // Number in double form. No reference is stored, so no barrier is owed.
    HeapValue* slots = elementsKeepingNumbers();
    for (uint32_t i = 0; i < length; ++i) {
        const Value v = slots[i];
        if (v.isInt32()) {
            slots[i] = Value::fromDouble(static_cast<double>(static_cast<int32_t>(v.payload())));
        }
    }
    reserved |= kElementsAreNumbers;
    syncElementsKind();
    return true;
}

}  // namespace bronze
