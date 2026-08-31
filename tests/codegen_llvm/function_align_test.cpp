// What the `BRONZE_XALIGN` seam promises (src/codegen-llvm/llvm_emit.h).
//
// Three properties, and the third is the one that makes it an INSTRUMENT
// rather than a codegen knob:
//
//   - a value it accepts reaches every emitted definition and nothing else;
//   - a value it does not accept is a diagnosed error, never a rounded or
//     ignored one, because a seam that silently measured a different alignment
//     than it was asked for would put a number in a bench log that is about
//     nothing;
//   - unset is not "align to 1", it is "say nothing at all" — no definition
//     carries an alignment it did not already have, which is what makes the
//     unset build byte-identical to one compiled without the seam.
//
// Stated against the parser and the applier directly rather than through the
// environment, because the variable is read into a process-wide static: a test
// cannot hold two values of it at once.

#include <doctest/doctest.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <string>

#include "codegen-llvm/llvm_emit.h"

using namespace bronze;

namespace {

llvm::Function* definition(llvm::Module& m, llvm::StringRef name) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(i64Ty, {i64Ty}, false),
                                      llvm::GlobalValue::ExternalLinkage, name, m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    b.CreateRet(fn->getArg(0));
    return fn;
}

llvm::Function* declaration(llvm::Module& m, llvm::StringRef name) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    return llvm::Function::Create(llvm::FunctionType::get(i64Ty, {i64Ty}, false),
                                  llvm::GlobalValue::ExternalLinkage, name, m);
}

}  // namespace

TEST_CASE("BRONZE_XALIGN accepts the power-of-two byte counts") {
    unsigned bytes = 99;
    std::string err = "stale";
    for (const char* value : {"1", "16", "32", "64", "128", "4096"}) {
        CHECK(codegen_llvm::parseFunctionAlign(value, bytes, err));
        CHECK(err.empty());
        CHECK(bytes == static_cast<unsigned>(std::stoul(value)));
    }
}

TEST_CASE("BRONZE_XALIGN unset asks for nothing") {
    unsigned bytes = 99;
    std::string err = "stale";
    CHECK(codegen_llvm::parseFunctionAlign(nullptr, bytes, err));
    CHECK(bytes == 0);
    CHECK(err.empty());
    CHECK(codegen_llvm::parseFunctionAlign("", bytes, err));
    CHECK(bytes == 0);
    CHECK(err.empty());
}

TEST_CASE("BRONZE_XALIGN diagnoses a value it cannot honour") {
    // Not a number; not a power of two; past a page; zero; trailing junk that
    // strtoul would otherwise swallow whole.
    for (const char* value : {"yes", "24", "8192", "0", "64k", "-16"}) {
        unsigned bytes = 99;
        std::string err;
        CHECK_FALSE(codegen_llvm::parseFunctionAlign(value, bytes, err));
        CHECK(bytes == 0);
        CHECK(err.find("BRONZE_XALIGN") != std::string::npos);
        CHECK(err.find(value) != std::string::npos);
    }
}

TEST_CASE("an alignment reaches every definition and no declaration") {
    llvm::LLVMContext ctx;
    llvm::Module m("xalign", ctx);
    llvm::Function* a = definition(m, "a");
    llvm::Function* b = definition(m, "b");
    llvm::Function* helper = declaration(m, "runtime_helper");

    // Nothing bronze emits sets a function alignment of its own, so "already
    // aligned" is the state the seam has to leave untouched at 0.
    CHECK_FALSE(a->getAlign().has_value());
    codegen_llvm::alignEmittedFunctions(m, 0);
    CHECK_FALSE(a->getAlign().has_value());
    CHECK_FALSE(b->getAlign().has_value());

    codegen_llvm::alignEmittedFunctions(m, 64);
    CHECK(a->getAlign() == llvm::Align(64));
    CHECK(b->getAlign() == llvm::Align(64));
    // A declaration has no placement to quantize — it is somebody else's body.
    CHECK_FALSE(helper->getAlign().has_value());
}
