// COLLECTION CALLS: `r = method.call %m, "get" | "has", %k, ...` whose method
// was always one collection's native: Map.prototype.get or has,
// Set.prototype.has, WeakMap.prototype.get, or the `has` WeakMap.prototype
// and WeakSet.prototype share (one native).
//
// Evidence (planCollection below): the site's one way was `direct` from the
// receiver shape the class makes its instances at (`Map{}`, `Set{}`,
// `WeakMap{}`, `WeakSet{}`), and its callee the native the site's key names
// on that class (ic_harvest.cpp calleeOf names it `intrinsic:Map.get`, ...).
// The op is chosen from the class and the key, never from the harvest's
// callee. The site becomes
//
//     guard is.collection %m, <class>
//     r = collection.op %m, %k, <op>
//
// Why exact: at the instance shape an object has no own property and the
// class's prototype, which lockdown froze, as its [[Prototype]], so [[Get]]
// of `get` or `has` reads the prototype's data property, which can only be
// the native; reading it runs nothing. The call is then the native's with
// the first argument, boxed if the function typed it (the rest are evaluated
// already and the native ignores them), and the native runs no user code: SameValueZero on the key, the
// table's probe, and its brand test, whose TypeError is the generic call's
// for an object that has the shape without the internal slots
// (`Object.create(Map.prototype)`). A failing guard has done nothing: it
// leaves in front of the site and G makes the call. What the form buys is
// the method read and the call protocol gone, and its effects: the generic
// call ends every fact about memory, where collection.op reads and writes
// only the table (il_op.h).

#include "abi/bronze_abi.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

// What a site's key reads at a class's instance shape, the op that is, and
// the name a harvest gives the native (`has` is one native for WeakMap and
// WeakSet).
struct CollectionNative {
    const char* receiver;
    const char* key;
    const char* callee;
    int32_t op;
    int32_t cls;
};

constexpr CollectionNative kNatives[] = {
    {"Map{}", "get", "intrinsic:Map.get", BRONZE_ABI_COLLECTION_MAP_GET, BRONZE_ABI_COLLECTION_MAP},
    {"Map{}", "has", "intrinsic:Map.has", BRONZE_ABI_COLLECTION_MAP_HAS, BRONZE_ABI_COLLECTION_MAP},
    {"Set{}", "has", "intrinsic:Set.has", BRONZE_ABI_COLLECTION_SET_HAS, BRONZE_ABI_COLLECTION_SET},
    {"WeakMap{}", "get", "intrinsic:WeakMap.get", BRONZE_ABI_COLLECTION_WEAKMAP_GET, BRONZE_ABI_COLLECTION_WEAKMAP},
    {"WeakMap{}", "has", "intrinsic:Weak.has", BRONZE_ABI_COLLECTION_WEAK_HAS, BRONZE_ABI_COLLECTION_WEAKMAP},
    {"WeakSet{}", "has", "intrinsic:Weak.has", BRONZE_ABI_COLLECTION_WEAK_HAS, BRONZE_ABI_COLLECTION_WEAKSET},
};

}  // namespace

// One way, at a class's instance shape, whose callee was the native the
// site's key names there, called with a key. The op comes from the class and
// the key, which is what the guard and the frozen prototype make exact; the
// harvest's callee only confirms it. A site whose receivers came in more
// shapes (a subclass, an instance given a property) is left to the other
// plans: those receivers would leave at every call.
std::optional<SitePlan> Evidence::planCollection(const spec::HarvestSite& h, const std::string& key, size_t argc,
                                                 BlockId block, uint32_t index) const {
    if (argc == 0 || h.full || h.mega || h.uncached > kMaxUncachedReceivers || h.ways.size() != 1) return std::nullopt;
    const spec::HarvestWay& way = h.ways.front();
    if (way.form != "direct") return std::nullopt;
    for (const CollectionNative& n : kNatives) {
        if (way.receiver != n.receiver || key != n.key || way.callee != n.callee) continue;
        SitePlan plan;
        plan.kind = SiteKind::CollectionCall;
        plan.block = block;
        plan.index = index;
        plan.intrinsic = n.op;
        plan.collectionClass = n.cls;
        return plan;
    }
    return std::nullopt;
}

void rewriteCollectionCall(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::CollectionCall);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(site.op == il::Op::MethodCall && site.operands.size() >= 2);
    const ValueId receiver = site.operands[0];
    b.guard(b.emit(il::Op::IsCollection, il::Type::Bool, {receiver}, plan.collectionClass));
    il::Instruction op;
    op.op = il::Op::CollectionOp;
    op.type = il::Type::Dynamic;
    op.operands = {receiver, b.box(site.operands[1])};
    op.immI32 = plan.intrinsic;
    if (site.result != il::kNoValue) {
        b.emitResult(std::move(op));
    } else {
        b.emitInst(std::move(op));
    }
    b.finish();
    ++ctx.stats.collectionCalls;
}

}  // namespace bronze::opt::speculate
