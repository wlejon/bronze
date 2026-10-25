#include "ast/type.h"

namespace bronze::ast {

const TypeDecl* TypeScope::lookup(std::string_view name) const {
    const std::string key(name);
    for (const TypeScope* s = this; s != nullptr; s = s->parent) {
        auto it = s->decls.find(key);
        if (it != s->decls.end()) return it->second.get();
    }
    return nullptr;
}

namespace {

void printTypeArgs(const std::vector<TypeNodePtr>& args, std::string& out);
void printTo(const TypeNode& t, std::string& out);

void printNode(const TypeNodePtr& t, std::string& out) {
    if (t) printTo(*t, out);
}

void printTypeParams(const std::vector<TypeParameter>& params, std::string& out) {
    if (params.empty()) return;
    out += '<';
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out += ", ";
        out += params[i].name;
        if (params[i].constraint) {
            out += " extends ";
            printTo(*params[i].constraint, out);
        }
        if (params[i].defaultType) {
            out += " = ";
            printTo(*params[i].defaultType, out);
        }
    }
    out += '>';
}

void printParams(const std::vector<TypeFnParam>& params, std::string& out) {
    out += '(';
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out += ", ";
        if (params[i].rest) out += "...";
        out += params[i].name;
        if (params[i].optional) out += '?';
        if (params[i].type) {
            if (!params[i].name.empty()) out += ": ";
            printTo(*params[i].type, out);
        }
    }
    out += ')';
}

void printMember(const TypeMember& m, std::string& out) {
    if (m.readonly) out += "readonly ";
    switch (m.kind) {
        case TypeMemberKind::Property:
            out += m.name;
            if (m.optional) out += '?';
            if (m.type) {
                out += ": ";
                printTo(*m.type, out);
            }
            return;
        case TypeMemberKind::Index:
            out += '[';
            if (!m.params.empty()) {
                out += m.params[0].name;
                if (m.params[0].type) {
                    out += ": ";
                    printTo(*m.params[0].type, out);
                }
            }
            out += ']';
            if (m.type) {
                out += ": ";
                printTo(*m.type, out);
            }
            return;
        case TypeMemberKind::Getter: out += "get "; break;
        case TypeMemberKind::Setter: out += "set "; break;
        case TypeMemberKind::Construct: out += "new "; break;
        case TypeMemberKind::Method:
        case TypeMemberKind::Call: break;
    }
    if (m.kind != TypeMemberKind::Call && m.kind != TypeMemberKind::Construct) {
        out += m.name;
        if (m.optional) out += '?';
    }
    printTypeParams(m.typeParams, out);
    printParams(m.params, out);
    if (m.type) {
        out += ": ";
        printTo(*m.type, out);
    }
}

void printJoined(const std::vector<TypeNodePtr>& types, const char* sep, std::string& out) {
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) out += sep;
        printNode(types[i], out);
    }
}

void printTypeArgs(const std::vector<TypeNodePtr>& args, std::string& out) {
    if (args.empty()) return;
    out += '<';
    printJoined(args, ", ", out);
    out += '>';
}

void printTo(const TypeNode& t, std::string& out) {
    switch (t.kind) {
        case TypeNodeKind::Keyword:
        case TypeNodeKind::Literal: out += t.name; return;
        case TypeNodeKind::Reference:
            out += t.name;
            printTypeArgs(t.types, out);
            return;
        case TypeNodeKind::Union: printJoined(t.types, " | ", out); return;
        case TypeNodeKind::Intersection: printJoined(t.types, " & ", out); return;
        case TypeNodeKind::Array:
            printNode(t.types[0], out);
            out += "[]";
            return;
        case TypeNodeKind::Tuple:
            out += '[';
            for (size_t i = 0; i < t.elements.size(); ++i) {
                const TypeTupleElement& e = t.elements[i];
                if (i > 0) out += ", ";
                if (e.rest) out += "...";
                if (!e.label.empty()) {
                    out += e.label;
                    if (e.optional) out += '?';
                    out += ": ";
                }
                printNode(e.type, out);
                if (e.optional && e.label.empty()) out += '?';
            }
            out += ']';
            return;
        case TypeNodeKind::Function:
            if (t.isAbstract) out += "abstract ";
            if (t.isConstructor) out += "new ";
            printTypeParams(t.typeParams, out);
            printParams(t.params, out);
            out += " => ";
            printNode(t.types[0], out);
            return;
        case TypeNodeKind::Object:
            if (t.members.empty()) {
                out += "{}";
                return;
            }
            out += "{ ";
            for (size_t i = 0; i < t.members.size(); ++i) {
                if (i > 0) out += "; ";
                printMember(t.members[i], out);
            }
            out += " }";
            return;
        case TypeNodeKind::Operator:
            out += t.name;
            out += ' ';
            printNode(t.types[0], out);
            return;
        case TypeNodeKind::Query:
            out += "typeof ";
            out += t.name;
            printTypeArgs(t.types, out);
            return;
        case TypeNodeKind::Indexed:
            printNode(t.types[0], out);
            out += '[';
            printNode(t.types[1], out);
            out += ']';
            return;
        case TypeNodeKind::Conditional:
            printNode(t.types[0], out);
            out += " extends ";
            printNode(t.types[1], out);
            out += " ? ";
            printNode(t.types[2], out);
            out += " : ";
            printNode(t.types[3], out);
            return;
        case TypeNodeKind::Infer:
            out += "infer ";
            out += t.name;
            if (!t.types.empty() && t.types[0]) {
                out += " extends ";
                printTo(*t.types[0], out);
            }
            return;
        case TypeNodeKind::Mapped: {
            out += "{ ";
            if (t.readonlyMod != 0) {
                if (t.readonlyMod != ' ') out += t.readonlyMod;
                out += "readonly ";
            }
            out += '[';
            if (!t.typeParams.empty()) {
                out += t.typeParams[0].name;
                out += " in ";
                printNode(t.typeParams[0].constraint, out);
            }
            if (t.types[0]) {
                out += " as ";
                printTo(*t.types[0], out);
            }
            out += ']';
            if (t.optionalMod != 0) {
                if (t.optionalMod != ' ') out += t.optionalMod;
                out += '?';
            }
            if (t.types[1]) {
                out += ": ";
                printTo(*t.types[1], out);
            }
            out += " }";
            return;
        }
        case TypeNodeKind::TemplateLiteral:
            out += '`';
            for (size_t i = 0; i < t.quasis.size(); ++i) {
                out += t.quasis[i];
                if (i < t.types.size()) {
                    out += "${";
                    printNode(t.types[i], out);
                    out += '}';
                }
            }
            out += '`';
            return;
        case TypeNodeKind::Import:
            out += "import(\"";
            out += t.name;
            out += "\")";
            for (const std::string& q : t.qualifier) {
                out += '.';
                out += q;
            }
            printTypeArgs(t.types, out);
            return;
        case TypeNodeKind::Predicate:
            if (t.asserts) out += "asserts ";
            out += t.name;
            if (!t.types.empty() && t.types[0]) {
                out += " is ";
                printTo(*t.types[0], out);
            }
            return;
        case TypeNodeKind::Parenthesized:
            out += '(';
            printNode(t.types[0], out);
            out += ')';
            return;
    }
}

}  // namespace

std::string printType(const TypeNode& type) {
    std::string out;
    printTo(type, out);
    return out;
}

bool namesType(const TypeNode& type, std::string_view name) {
    switch (type.kind) {
        case TypeNodeKind::Reference: return type.name == name;
        case TypeNodeKind::Parenthesized:
            return type.types[0] && namesType(*type.types[0], name);
        case TypeNodeKind::Union:
            for (const auto& member : type.types) {
                if (member && namesType(*member, name)) return true;
            }
            return false;
        default: return false;
    }
}

}  // namespace bronze::ast
