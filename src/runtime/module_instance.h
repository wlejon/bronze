#pragma once

#include <cstdint>

namespace bronze::runtime {

// Whether the calling thread's CURRENT REALM has registered its instance of
// the module whose slot cell is `slotCell` (bronze_abi.h,
// `bronze_module_instance`): it has run the module's entry, so its copy of the
// module's data is no longer the pristine one a first run starts from.
bool rtThreadHasModuleInstance(const uint64_t* slotCell);
void rtTeardownThreadModuleInstances();
// The program loaded under `epoch` is being unloaded on this thread
// (rtDropModuleEpoch): every instance its entries made, in any realm, is put
// back as it was before an entry wrote to it, so a later entry of the same
// image in that realm is a first entry. Called while the instance's memory is
// still there.
void rtResetModuleInstancesOfEpoch(uint64_t epoch);
// The modules loaded under `epoch` were reclaimed by the collector, memory and
// all (rt_state.cpp, sweepDynamicModules): their instances are no longer that
// epoch's to reset.
void rtForgetModuleInstancesOfEpoch(uint64_t epoch);

// Make the instances of realm `realmSlot` the ones generated code addresses
// (the TLS block's delta array). 0 is the thread's own table, which its
// default realm uses. realm.cpp calls this on every enter, exit and destroy.
void rtActivateRealmModuleInstances(uint32_t realmSlot);
// The realm is being destroyed: drop the GC registrations that point into its
// instances and its function singletons, then free the instances. Not the
// active table (the caller activated another first). 0 is ignored.
void rtReleaseRealmModuleInstances(uint32_t realmSlot);

}  // namespace bronze::runtime
