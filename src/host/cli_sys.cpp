#include "host/cli_sys.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#else
#include <climits>
#include <unistd.h>
#endif

#include "embed/embed.h"
#include "host/cli_proc.h"

namespace bronze::host {
namespace {

namespace fs = std::filesystem;
namespace embed = bronze::embed;

fs::path utf8Path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string requireString(std::span<const embed::Value> args, size_t idx, const char* fnName) {
    if (idx >= args.size() || !embed::isString(args[idx])) {
        embed::throwTypeError(std::string(fnName) + ": expected string argument at index " +
                              std::to_string(idx));
    }
    return embed::toUtf8(args[idx]);
}

std::vector<std::string> parseStringArray(embed::Value v, const char* fnName) {
    if (!embed::isObject(v)) {
        embed::throwTypeError(std::string(fnName) + ": expected array of strings");
    }
    embed::Persistent arr(v);
    embed::Value lenVal = embed::getProperty(arr.get(), "length");
    if (!embed::isNumber(lenVal)) {
        embed::throwTypeError(std::string(fnName) + ": expected array of strings");
    }
    const double lenD = embed::toDouble(lenVal);
    if (lenD < 0 || lenD > 1000000.0) {
        embed::throwRangeError(std::string(fnName) + ": invalid array length");
    }
    const uint32_t len = static_cast<uint32_t>(lenD);
    std::vector<std::string> out;
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        embed::Value elem = embed::getElement(arr.get(), i);
        if (!embed::isString(elem)) {
            embed::throwTypeError(std::string(fnName) + ": command element " + std::to_string(i) +
                                  " must be a string");
        }
        out.push_back(embed::toUtf8(elem));
    }
    return out;
}

ProcOptions parseProcOptions(std::span<const embed::Value> args, size_t idx, const char* fnName) {
    ProcOptions opts;
    if (idx >= args.size() || embed::isUndefined(args[idx]) || embed::isNull(args[idx])) {
        return opts;
    }
    if (!embed::isObject(args[idx])) {
        embed::throwTypeError(std::string(fnName) + ": options must be an object");
    }
    embed::Persistent o(args[idx]);

    embed::Value cwdVal = embed::getProperty(o.get(), "cwd");
    if (!embed::isUndefined(cwdVal) && !embed::isNull(cwdVal)) {
        if (!embed::isString(cwdVal)) {
            embed::throwTypeError(std::string(fnName) + ": options.cwd must be a string");
        }
        opts.cwd = embed::toUtf8(cwdVal);
    }

    embed::Value timeoutVal = embed::getProperty(o.get(), "timeoutMs");
    if (!embed::isUndefined(timeoutVal) && !embed::isNull(timeoutVal)) {
        if (!embed::isNumber(timeoutVal)) {
            embed::throwTypeError(std::string(fnName) + ": options.timeoutMs must be a number");
        }
        opts.timeoutMs = embed::toInt64(timeoutVal);
    }

    embed::Value mergeVal = embed::getProperty(o.get(), "mergeStderr");
    if (!embed::isUndefined(mergeVal) && !embed::isNull(mergeVal)) {
        opts.mergeStderr = embed::toBool(mergeVal);
    }

    embed::Value liveVal = embed::getProperty(o.get(), "liveLogPath");
    if (!embed::isUndefined(liveVal) && !embed::isNull(liveVal)) {
        if (!embed::isString(liveVal)) {
            embed::throwTypeError(std::string(fnName) + ": options.liveLogPath must be a string");
        }
        opts.liveLogPath = embed::toUtf8(liveVal);
    }

    embed::Value stopVal = embed::getProperty(o.get(), "stopOnMatch");
    if (!embed::isUndefined(stopVal) && !embed::isNull(stopVal)) {
        opts.stopOnMatch = parseStringArray(stopVal, fnName);
    }

    embed::Value envVal = embed::getProperty(o.get(), "env");
    if (!embed::isUndefined(envVal) && !embed::isNull(envVal)) {
        if (!embed::isObject(envVal)) {
            embed::throwTypeError(std::string(fnName) + ": options.env must be an object");
        }
        embed::Persistent envP(envVal);
        embed::GlobalValue objG = embed::globalValue("Object");
        if (!objG.found) embed::throwError("internal: Object global missing");
        embed::Persistent objP(objG.value);
        embed::Persistent entriesFn(embed::getProperty(objP.get(), "entries"));
        embed::Value argArr[1] = {envP.get()};
        embed::CallResult cr = embed::call(entriesFn.get(), objP.get(), argArr);
        if (cr.thrown) embed::throwValue(cr.value);
        embed::Persistent pairs(cr.value);
        const uint32_t n = static_cast<uint32_t>(embed::toDouble(embed::getProperty(pairs.get(), "length")));
        opts.envOverrides.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            embed::Persistent pair(embed::getElement(pairs.get(), i));
            embed::Value kVal = embed::getElement(pair.get(), 0);
            embed::Value vVal = embed::getElement(pair.get(), 1);
            if (embed::isUndefined(vVal) || embed::isNull(vVal)) continue;
            std::string k = embed::toUtf8(kVal);
            std::string v = embed::toUtf8(vVal);
            opts.envOverrides.emplace_back(std::move(k), std::move(v));
        }
    }
    return opts;
}

embed::Value procResultToJs(const ProcResult& r, bool includeId) {
    if (!r.spawnError.empty()) {
        embed::throwError(r.spawnError);
    }
    embed::ObjectBuilder b;
    if (includeId) b.set("id", static_cast<double>(r.id));
    b.set("status", r.exitCode);
    b.set("stdout", r.stdoutText);
    b.set("stderr", r.stderrText);
    b.set("timedOut", r.timedOut);
    b.set("stoppedEarly", r.stoppedEarly);
    b.set("marks", r.marks);
    return b.build();
}

embed::Value buildFsNamespace() {
    embed::ObjectBuilder f;

    f.def("readText", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.readText");
        std::ifstream in(utf8Path(p), std::ios::binary);
        if (!in) embed::throwError("Bronze.fs.readText: cannot open " + p);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) embed::throwError("Bronze.fs.readText: read failed for " + p);
        return embed::fromUtf8(content);
    });

    f.def("writeText", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.writeText");
        const std::string text = requireString(args, 1, "Bronze.fs.writeText");
        std::ofstream out(utf8Path(p), std::ios::binary | std::ios::trunc);
        if (!out) embed::throwError("Bronze.fs.writeText: cannot open " + p);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) embed::throwError("Bronze.fs.writeText: write failed for " + p);
        return embed::undefined();
    });

    f.def("readBytes", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.readBytes");
        std::ifstream in(utf8Path(p), std::ios::binary);
        if (!in) embed::throwError("Bronze.fs.readBytes: cannot open " + p);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) embed::throwError("Bronze.fs.readBytes: read failed for " + p);
        embed::Value arr = embed::createTypedArray(embed::elements::Uint8,
                                                   static_cast<uint32_t>(content.size()));
        if (!content.empty()) {
            embed::TypedArrayInfo info = embed::typedArrayInfo(arr);
            std::memcpy(info.data, content.data(), content.size());
        }
        return arr;
    });

    f.def("writeBytes", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.writeBytes");
        if (args.size() < 2) embed::throwTypeError("Bronze.fs.writeBytes: expected Uint8Array or ArrayBuffer");
        const uint8_t* ptr = nullptr;
        size_t len = 0;
        if (embed::TypedArrayInfo ta = embed::typedArrayInfo(args[1])) {
            ptr = ta.data;
            len = ta.byteLength;
        } else if (embed::ArrayBufferInfo ab = embed::arrayBufferInfo(args[1])) {
            ptr = ab.data;
            len = ab.byteLength;
        } else {
            embed::throwTypeError("Bronze.fs.writeBytes: expected TypedArray or ArrayBuffer");
        }
        std::ofstream out(utf8Path(p), std::ios::binary | std::ios::trunc);
        if (!out) embed::throwError("Bronze.fs.writeBytes: cannot open " + p);
        if (len > 0) out.write(reinterpret_cast<const char*>(ptr), static_cast<std::streamsize>(len));
        if (!out) embed::throwError("Bronze.fs.writeBytes: write failed for " + p);
        return embed::undefined();
    });

    f.def("exists", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.exists");
        std::error_code ec;
        return embed::fromBool(fs::exists(utf8Path(p), ec));
    });

    f.def("isDir", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.isDir");
        std::error_code ec;
        return embed::fromBool(fs::is_directory(utf8Path(p), ec));
    });

    f.def("isFile", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.isFile");
        std::error_code ec;
        return embed::fromBool(fs::is_regular_file(utf8Path(p), ec));
    });

    f.def("stat", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.stat");
        const fs::path fp = utf8Path(p);
        std::error_code ec;
        const fs::file_status st = fs::status(fp, ec);
        if (ec || !fs::exists(st)) return embed::null();
        const bool isDir = fs::is_directory(st);
        const bool isFile = fs::is_regular_file(st);
        const uint64_t sz = isFile ? fs::file_size(fp, ec) : 0;
        double mtimeMs = 0.0;
        const auto ftime = fs::last_write_time(fp, ec);
        if (!ec) {
            const auto fnow = fs::file_time_type::clock::now();
            const auto snow = std::chrono::system_clock::now();
            const auto spt = snow + std::chrono::duration_cast<std::chrono::system_clock::duration>(ftime - fnow);
            mtimeMs = static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(spt.time_since_epoch()).count());
        }
        embed::ObjectBuilder res;
        res.set("size", static_cast<double>(sz));
        res.set("mtimeMs", mtimeMs);
        res.set("isDir", isDir);
        res.set("isFile", isFile);
        return res.build();
    });

    f.def("mkdir", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.mkdir");
        std::error_code ec;
        fs::create_directories(utf8Path(p), ec);
        if (ec) embed::throwError("Bronze.fs.mkdir: " + p + ": " + ec.message());
        return embed::undefined();
    });

    f.def("rm", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.rm");
        std::error_code ec;
        fs::remove_all(utf8Path(p), ec);
        return embed::undefined();
    });

    f.def("unlink", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.unlink");
        std::error_code ec;
        fs::remove(utf8Path(p), ec);
        return embed::undefined();
    });

    f.def("rename", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string from = requireString(args, 0, "Bronze.fs.rename");
        const std::string to = requireString(args, 1, "Bronze.fs.rename");
        std::error_code ec;
        fs::rename(utf8Path(from), utf8Path(to), ec);
        if (ec) embed::throwError("Bronze.fs.rename: " + ec.message());
        return embed::undefined();
    });

    f.def("listDir", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.fs.listDir");
        std::error_code ec;
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(utf8Path(p), ec)) {
            names.push_back(pathToUtf8(entry.path().filename()));
        }
        if (ec) embed::throwError("Bronze.fs.listDir: " + p + ": " + ec.message());
        std::sort(names.begin(), names.end());
        embed::ArrayBuilder arr(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            arr.set(static_cast<uint32_t>(i), names[i]);
        }
        return arr.build();
    });

    f.def("tempDir", 0, [](embed::Value, std::span<const embed::Value>) -> embed::Value {
        std::error_code ec;
        const fs::path td = fs::temp_directory_path(ec);
        if (ec) embed::throwError("Bronze.fs.tempDir: " + ec.message());
        return embed::fromUtf8(pathToUtf8(td));
    });

    f.def("symlink", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string target = requireString(args, 0, "Bronze.fs.symlink");
        const std::string link = requireString(args, 1, "Bronze.fs.symlink");
        const fs::path tp = utf8Path(target);
        const fs::path lp = utf8Path(link);
        std::error_code ec;
        if (fs::is_directory(tp, ec)) fs::create_directory_symlink(tp, lp, ec);
        else fs::create_symlink(tp, lp, ec);
        if (ec) embed::throwError("Bronze.fs.symlink: " + ec.message());
        return embed::undefined();
    });

    return embed::freeze(f.build());
}

embed::Value buildStdinNamespace() {
    embed::ObjectBuilder s;
    auto readAllStdin = []() -> std::string {
#if defined(_WIN32)
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        std::string out;
        char buf[8192];
        while (true) {
            const size_t n = std::fread(buf, 1, sizeof(buf), stdin);
            if (n > 0) out.append(buf, n);
            if (n < sizeof(buf)) break;
        }
        return out;
    };
    s.def("readText", 0, [readAllStdin](embed::Value, std::span<const embed::Value>) -> embed::Value {
        return embed::fromUtf8(readAllStdin());
    });
    s.def("readBytes", 0, [readAllStdin](embed::Value, std::span<const embed::Value>) -> embed::Value {
        const std::string data = readAllStdin();
        embed::Value arr = embed::createTypedArray(embed::elements::Uint8, static_cast<uint32_t>(data.size()));
        if (!data.empty()) {
            embed::TypedArrayInfo info = embed::typedArrayInfo(arr);
            std::memcpy(info.data, data.data(), data.size());
        }
        return arr;
    });
    return embed::freeze(s.build());
}

embed::Value buildPathNamespace() {
    embed::ObjectBuilder pb;

    pb.def("join", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        fs::path acc;
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string part = requireString(args, i, "Bronze.path.join");
            if (i == 0) acc = utf8Path(part);
            else acc /= utf8Path(part);
        }
        return embed::fromUtf8(pathToUtf8(acc.lexically_normal()));
    });

    pb.def("resolve", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        fs::path acc;
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string part = requireString(args, i, "Bronze.path.resolve");
            const fs::path fp = utf8Path(part);
            if (i == 0 || fp.is_absolute()) acc = fp;
            else acc /= fp;
        }
        std::error_code ec;
        fs::path abs = fs::weakly_canonical(acc, ec);
        if (ec) abs = fs::absolute(acc, ec);
        return embed::fromUtf8(pathToUtf8(ec ? acc.lexically_normal() : abs));
    });

    pb.def("dirname", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.path.dirname");
        return embed::fromUtf8(pathToUtf8(utf8Path(p).parent_path()));
    });

    pb.def("basename", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.path.basename");
        return embed::fromUtf8(pathToUtf8(utf8Path(p).filename()));
    });

    pb.def("extname", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.path.extname");
        return embed::fromUtf8(pathToUtf8(utf8Path(p).extension()));
    });

    pb.def("stem", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.path.stem");
        return embed::fromUtf8(pathToUtf8(utf8Path(p).stem()));
    });

    pb.def("isAbsolute", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string p = requireString(args, 0, "Bronze.path.isAbsolute");
        return embed::fromBool(utf8Path(p).is_absolute());
    });

    pb.def("relative", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        const std::string from = requireString(args, 0, "Bronze.path.relative");
        const std::string to = requireString(args, 1, "Bronze.path.relative");
        std::error_code ec;
        fs::path rel = fs::relative(utf8Path(to), utf8Path(from), ec);
        if (ec) rel = utf8Path(to).lexically_relative(utf8Path(from));
        return embed::fromUtf8(pathToUtf8(rel));
    });

    return embed::freeze(pb.build());
}

}  // namespace

std::string ownExecutablePath() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (len == 0 || len >= sizeof(buffer) / sizeof(buffer[0])) return {};
    return pathToUtf8(fs::path(buffer));
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) return {};
    char real[PATH_MAX];
    if (realpath(buffer, real) != nullptr) return std::string(real);
    return std::string(buffer);
#else
    char buffer[PATH_MAX];
    const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) return {};
    buffer[len] = '\0';
    return std::string(buffer);
#endif
}

void installCliHost(CliHostConfig cfg) {
    embed::registerLazyGlobal("Bronze", [cfg = std::move(cfg)]() {
        embed::ObjectBuilder b;

        embed::ArrayBuilder argArr(cfg.args.size());
        for (size_t i = 0; i < cfg.args.size(); ++i) {
            argArr.set(static_cast<uint32_t>(i), cfg.args[i]);
        }
        b.defineConstant("args", embed::freeze(argArr.build()));
        b.defineConstant("exe", embed::fromUtf8(cfg.exePath));
#if defined(_WIN32)
        b.defineConstant("platform", embed::fromUtf8("windows"));
#elif defined(__APPLE__)
        b.defineConstant("platform", embed::fromUtf8("macos"));
#else
        b.defineConstant("platform", embed::fromUtf8("linux"));
#endif

        b.def("cpuCount", 0, [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            const unsigned n = std::thread::hardware_concurrency();
            return embed::fromDouble(static_cast<double>(n ? n : 1));
        });

        b.def("cwd", 0, [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            std::error_code ec;
            const fs::path p = fs::current_path(ec);
            if (ec) embed::throwError("Bronze.cwd: " + ec.message());
            return embed::fromUtf8(pathToUtf8(p));
        });

        b.def("getenv", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            const std::string name = requireString(args, 0, "Bronze.getenv");
#if defined(_WIN32)
            char* buf = nullptr;
            size_t len = 0;
            if (_dupenv_s(&buf, &len, name.c_str()) != 0 || buf == nullptr) {
                return embed::undefined();
            }
            std::string val(buf);
            std::free(buf);
            return embed::fromUtf8(val);
#else
            const char* val = std::getenv(name.c_str());
            if (!val) return embed::undefined();
            return embed::fromUtf8(val);
#endif
        });

        auto beforeExit = cfg.beforeExit;
        b.def("exit", 1, [beforeExit](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            int code = 0;
            if (!args.empty() && !embed::isUndefined(args[0])) {
                code = static_cast<int>(embed::toInt64(args[0]));
            }
            std::fflush(stdout);
            std::fflush(stderr);
            embed::writeInlineCacheHarvests();
            if (beforeExit) beforeExit();
            std::exit(code);
        });

        b.def("drainMicrotasks", 0, [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            embed::drainMicrotasks();
            return embed::undefined();
        });

        b.def("sleep", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            if (args.empty() || !embed::isNumber(args[0])) {
                embed::throwTypeError("Bronze.sleep: expected milliseconds number");
            }
            const int64_t ms = embed::toInt64(args[0]);
            if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return embed::undefined();
        });

        b.defineConstant("fs", buildFsNamespace());
        b.defineConstant("path", buildPathNamespace());
        b.defineConstant("stdin", buildStdinNamespace());

        b.def("exec", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            if (args.empty()) embed::throwTypeError("Bronze.exec: expected command array");
            const std::vector<std::string> cmd = parseStringArray(args[0], "Bronze.exec");
            const ProcOptions opts = parseProcOptions(args, 1, "Bronze.exec");
            const ProcResult res = execProcess(cmd, opts);
            return procResultToJs(res, /*includeId=*/false);
        });

        b.def("execPerfMarked", 3, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            if (args.size() < 2) {
                embed::throwTypeError("Bronze.execPerfMarked: expected (perfPrefix, childCmd, opts?)");
            }
            const std::vector<std::string> perfPrefix = parseStringArray(args[0], "Bronze.execPerfMarked");
            const std::vector<std::string> childCmd = parseStringArray(args[1], "Bronze.execPerfMarked");
            const ProcOptions opts = parseProcOptions(args, 2, "Bronze.execPerfMarked");
            std::string markOn = "RW mark on";
            std::string markOff = "RW mark off";
            if (args.size() >= 3 && embed::isObject(args[2])) {
                embed::Persistent o(args[2]);
                embed::Value onV = embed::getProperty(o.get(), "markOn");
                if (embed::isString(onV)) markOn = embed::toUtf8(onV);
                embed::Value offV = embed::getProperty(o.get(), "markOff");
                if (embed::isString(offV)) markOff = embed::toUtf8(offV);
            }
            const ProcResult res = execPerfMarked(perfPrefix, childCmd, markOn, markOff, opts);
            return procResultToJs(res, /*includeId=*/false);
        });

        b.def("spawn", 2, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            if (args.empty()) embed::throwTypeError("Bronze.spawn: expected command array");
            const std::vector<std::string> cmd = parseStringArray(args[0], "Bronze.spawn");
            const ProcOptions opts = parseProcOptions(args, 1, "Bronze.spawn");
            const uint64_t id = spawnProcess(cmd, opts);
            return embed::fromDouble(static_cast<double>(id));
        });

        b.def("wait", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            if (args.empty() || !embed::isNumber(args[0])) {
                embed::throwTypeError("Bronze.wait: expected handle id");
            }
            const uint64_t id = embed::toUint64(args[0]);
            const ProcResult res = waitProcess(id);
            return procResultToJs(res, /*includeId=*/true);
        });

        b.def("waitAny", 0, [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            const std::optional<ProcResult> res = waitAnyProcess();
            if (!res) return embed::null();
            return procResultToJs(*res, /*includeId=*/true);
        });

        embed::registerGlobal("Bronze", embed::freeze(b.build()), embed::GlobalKind::Readonly);
    });
}

}  // namespace bronze::host
