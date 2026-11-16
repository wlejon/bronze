// The names a class installs on its instances, counted whatever its layout
// verdict (class_layout.h `ownNames`).
//
// A class whose layout is refused still makes instances, and how many Values
// they are allocated inline decides whether a property access reads the
// object or the overflow block behind it, and whether a slot can hold a
// double at all (BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT counts slot numbers, and
// overflow slots are numbered from BRONZE_ABI_OBJ_OVERFLOW_SLOT). The common
// refusal is a chain that reaches a constructor FUNCTION (pixi's classes all
// extend eventemitter3's `EventEmitter`): every instance of every such class
// was allocated the default four Values, and a Sprite kept 58 of its 62
// properties in overflow. The count is a capacity, not a layout: a name
// counted that an instance never gets wastes a slot; one missed lands in
// overflow, as before.

#include <set>

#include "types/class_layout.h"
#include "types/this_write_walker.h"

namespace bronze::types {

void ClassLayoutTable::collectOwnNames() {
    for (size_t i = 0; i < classes_.size(); ++i) {
        const std::vector<ast::ClassMethod>* methods = methodsByIndex_[i];
        if (methods == nullptr) continue;
        std::vector<std::string>& out = classes_[i].ownNames;
        std::set<std::string> seen;
        auto add = [&](const std::string& name) {
            if (!name.empty() && seen.insert(name).second) out.push_back(name);
        };
        for (const ast::ClassMethod& m : *methods) {
            if (m.isStatic) continue;
            if (m.isField) {
                if (!m.computed() && !m.isPrivate()) add(m.name);
                continue;
            }
            if (!m.fn) continue;
            detail::ThisWriteWalker walker;
            walker.methods = methods;
            walker.walkList(m.fn->body);
            for (const std::string& name : walker.names) add(name);
            for (const std::string& name : walker.lateFields) add(name);
        }
    }
}

}  // namespace bronze::types
