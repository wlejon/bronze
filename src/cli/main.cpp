#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "support/diagnostics.h"

namespace {

constexpr const char* kUsage =
    "bronze — AOT compiler for JavaScript (native-first, LLVM backend)\n"
    "\n"
    "Usage:\n"
    "  bronze lex <file>     Tokenize and print one token per line\n"
    "  bronze parse <file>   Parse and print the canonical AST dump\n"
    "  bronze version        Print version\n";

int fail(const std::string& message) {
    std::fputs(message.c_str(), stderr);
    return 1;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return fail(kUsage);
    const std::string command = argv[1];

    if (command == "version") {
        std::puts("bronze 0.1.0");
        return 0;
    }

    if (command == "lex" || command == "parse") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string text;
        if (!readFile(argv[2], text)) return fail(std::string("error: cannot read ") + argv[2] + "\n");
        bronze::SourceBuffer buffer(argv[2], std::move(text));
        bronze::DiagnosticSink diags;
        auto tokens = bronze::Lexer(buffer, diags).lex();
        if (diags.hasErrors()) return fail(diags.render(buffer));

        if (command == "lex") {
            for (const auto& t : tokens) {
                std::printf("%s\t%.*s\n", bronze::tokenKindName(t.kind),
                            static_cast<int>(t.text.size()), t.text.data());
            }
            return 0;
        }

        auto module = bronze::Parser(std::move(tokens), diags).parseModule(argv[2]);
        if (diags.hasErrors() || !module) return fail(diags.render(buffer));
        std::fputs(bronze::ast::dump(*module).c_str(), stdout);
        return 0;
    }

    return fail(kUsage);
}
