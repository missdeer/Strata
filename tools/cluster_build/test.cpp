#include "build.hpp"
#include "strata/runtime/cluster_launch.hpp"
#include <fstream>
#include <chrono>
#include <iostream>
#include <functional>

using namespace strata::build;
namespace {
void check(bool condition) { if (!condition) throw std::runtime_error("check failed"); }
void fails(const std::function<void()>& operation, const std::string& fragment = "") {
    try { operation(); } catch (const std::exception& error) {
        check(std::string(error.what()).find(fragment) != std::string::npos); return;
    }
    throw std::runtime_error("expected failure");
}
void write(const fs::path& path, const std::string& text = "") { std::ofstream out(path, std::ios::binary); out << text; }
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("strata-builder-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() { fs::create_directory(path); }
    ~Temporary() { std::error_code error; fs::remove_all(path, error); }
};
Cache portable() {
#ifdef _WIN32
    const std::string compiler = "C:/Intel/icx-cl.exe", flags = "/O3 /DNDEBUG";
#else
    const std::string compiler = "/opt/intel/icpx", flags = "-O3 -DNDEBUG";
#endif
    return {{"STRATA_ENABLE_CUDA", "OFF"}, {"STRATA_ENABLE_HIP", "OFF"}, {"STRATA_PORTABLE", "ON"},
        {"CMAKE_CXX_COMPILER", compiler}, {"CMAKE_CXX_FLAGS_RELEASE", flags}, {"GGML_NATIVE", "OFF"}, {"STRATA_BUILD_NATIVE_CLUSTER", "ON"}, {"CMAKE_BUILD_TYPE", "Release"}};
}
std::vector<unsigned char> pe(const std::vector<std::string>& dependencies, bool delayed = false) {
    std::vector<unsigned char> bytes(4096);
    auto put = [&](size_t offset, uint64_t value, size_t length = 4) {
        for (size_t i = 0; i < length; ++i) bytes.at(offset + i) = static_cast<unsigned char>(value >> (8 * i));
    };
    put(0, 0x5a4d, 2); put(0x3c, 0x80); put(0x80, 0x4550);
    put(0x84, 0x8664, 2); put(0x86, 1, 2); put(0x94, 240, 2); put(0x98, 0x20b, 2); put(0x98 + 108, 16);
    const size_t directory = delayed ? 13 : 1, stride = delayed ? 32 : 20;
    put(0x98 + 112 + directory * 8, 0x1000); put(0x98 + 116 + directory * 8, (dependencies.size() + 1) * stride);
    const size_t section = 0x98 + 240;
    put(section + 8, 0xe00); put(section + 12, 0x1000); put(section + 16, 0xe00); put(section + 20, 0x200);
    size_t name_offset = 0x600;
    for (size_t i = 0; i < dependencies.size(); ++i) {
        if (delayed) put(0x200 + i * stride, 1);
        put(0x200 + i * stride + (delayed ? 4 : 12), name_offset + 0xe00);
        for (char c : dependencies[i]) bytes.at(name_offset++) = static_cast<unsigned char>(c);
        bytes.at(name_offset++) = 0;
    }
    return bytes;
}
void binary(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
struct PackageFixture {
    Temporary temporary;
    Options options;
    PackageFixture() {
        options.root = temporary.path / "source"; options.out_root = temporary.path / "out";
        options.hosts = {"host11"}; options.package_only = true;
        const auto build = temporary.path / "build"; options.builds["host11"] = build;
        fs::create_directories(build / "native-text-licenses");
        write(build / "native-text-licenses/test-LICENSE.txt", "license");
        fs::create_directories(options.root / "serve/web"); fs::create_directory(options.root / "configs");
        write(options.root / "LICENSE", "license"); write(options.root / "serve/chat_template.jinja", "template");
        write(options.root / "serve/web/index.html", "web");
        for (const auto& name : {"cluster-2hosts.json", "cluster-3hosts.json", "cluster-4hosts.example.json"}) write(options.root / "configs" / name, "{}");
        std::string cache;
        for (const auto& [name, value] : portable()) cache += name + ":STRING=" + value + "\n";
        write(build / "CMakeCache.txt", cache);
        binary(build / "strata-host.exe", pe({"KERNEL32.dll"})); binary(build / "strata_network_backend.dll", pe({}));
    }
};
std::vector<unsigned char> elf(const std::vector<std::string>& dependencies) {
    std::vector<unsigned char> bytes(2048);
    auto put = [&](size_t offset, uint64_t value, size_t length = 8) {
        for (size_t i = 0; i < length; ++i) bytes.at(offset + i) = static_cast<unsigned char>(value >> (8 * i));
    };
    put(0, 0x464c457f, 4); put(4, 2, 1); put(5, 1, 1); put(18, 62, 2); put(32, 64); put(54, 56, 2); put(56, 2, 2);
    put(64, 1, 4); put(72, 0); put(80, 0x400000); put(96, bytes.size());
    put(120, 2, 4); put(128, 0x200); put(152, (dependencies.size() + 3) * 16);
    put(0x200, 5); put(0x208, 0x400500); put(0x210, 10); put(0x218, 512);
    size_t cursor = 0;
    for (size_t i = 0; i < dependencies.size(); ++i) {
        put(0x220 + i * 16, 1); put(0x228 + i * 16, cursor);
        for (char c : dependencies[i]) bytes.at(0x500 + cursor++) = static_cast<unsigned char>(c);
        bytes.at(0x500 + cursor++) = 0;
    }
    return bytes;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc > 1) { std::cout << "native child output\n"; return 7; }
#else
int main(int argc, char**) {
    if (argc > 1) { std::cout << "native child output\n"; return 7; }
#endif
    int passed = 0, failed = 0;
    auto test = [&](const char* name, const std::function<void()>& run) {
        try { run(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    test("portable profile", [] { verify_profile("host11", portable(), {}); });
    test("reject host ISA leakage", [] {
        for (const auto& option : {"/QxHost", "/QxCORE-AVX512", "/arch:AVX512", "/arch:AVX10.1", "-mavx512f", "-mavxvnni", "-march=native", "-mcpu=native", "-xHost"}) {
            auto cache = portable(); cache["CMAKE_C_FLAGS"] = option; fails([&] { verify_profile("host11", cache, {}); }, "nonportable");
        }
    });
    test("reject GPU or native ggml on CPU node", [] {
        for (const auto& name : {"STRATA_ENABLE_CUDA", "STRATA_ENABLE_HIP", "GGML_NATIVE", "GGML_AVX512", "GGML_AVX_VNNI", "GGML_AMX_TILE"}) {
            auto cache = portable(); cache[name] = "ON"; fails([&] { verify_profile("host11", cache, {}); });
        }
    });
    test("reject stale HIP architecture", [] {
        auto cache = portable(); cache["STRATA_ENABLE_HIP"] = "ON"; cache["CMAKE_CXX_COMPILER"] = "clang++.exe";
        cache["CMAKE_HIP_ARCHITECTURES"] = "gfx1102"; fails([&] { verify_profile("host17", cache, {"gfx1102", "gfx1103"}); }, "HIP targets");
    });
    test("native switch required", [] { auto cache = portable(); cache["STRATA_BUILD_NATIVE_CLUSTER"] = "OFF"; fails([&] { verify_profile("host11", cache, {}); }); });
    test("reject debug products", [] { auto cache = portable(); cache["CMAKE_BUILD_TYPE"] = "Debug"; fails([&] { verify_profile("host11", cache, {}); }, "Release"); });
    test("PE ordinary and delay imports", [] {
        Temporary tmp;
        for (bool delayed : {false, true}) { auto file = tmp.path / "test.exe"; binary(file, pe({"a.dll", "B.dll"}, delayed)); check(imports(file) == std::vector<std::string>{"a.dll", "B.dll"}); }
    });
    test("malformed binary fails closed", [] {
        Temporary tmp; auto file = tmp.path / "test.exe";
        write(file, "MZ"); fails([&] { imports(file); });
        auto bytes = pe({"a.dll"}); bytes.resize(300); binary(file, bytes); fails([&] { imports(file); });
        write(file, "invalid"); fails([&] { imports(file); }, "not a PE/ELF");
    });
    test("reject import traversal", [] {
        Temporary tmp; auto file = tmp.path / "test.exe"; binary(file, pe({"../outside.dll"})); fails([&] { imports(file); }, "import name");
    });
    test("ELF dynamic imports", [] {
        Temporary tmp; auto file = tmp.path / "test.so"; binary(file, elf({"liba.so.1", "libb.so.2"}));
        check(imports(file) == std::vector<std::string>{"liba.so.1", "libb.so.2"});
        auto bytes = elf({"liba.so.1"}); bytes[0x228] = 0xff; bytes[0x229] = 0x7f; binary(file, bytes); fails([&] { imports(file); }, "outside string");
    });
    test("recursive dependency closure", [] {
        Temporary tmp; auto sdk = tmp.path / "sdk", stage = tmp.path / "stage"; fs::create_directory(sdk); fs::create_directory(stage);
        auto program = stage / "program.exe";
#ifdef _WIN32
        binary(program, pe({"A.dll", "KERNEL32.dll", "WINHTTP.dll", "pdh.dll"})); binary(sdk / "a.dll", pe({"b.dll"})); binary(sdk / "b.dll", pe({"a.dll", "WS2_32.dll"}));
        copy_runtime(stage, {program}, {sdk}); check(fs::exists(stage / "b.dll")); check(!fs::exists(stage / "KERNEL32.dll"));
#else
        binary(program, elf({"liba.so.1", "libc.so.6"})); binary(sdk / "liba.so.1", elf({"libb.so.2"})); binary(sdk / "libb.so.2", elf({"liba.so.1"}));
        copy_runtime(stage, {program}, {sdk}); check(fs::exists(stage / "libb.so.2")); check(!fs::exists(stage / "libc.so.6"));
#endif
    });
    test("missing runtime aborts", [] { Temporary tmp; auto file = tmp.path / "program.exe"; binary(file, pe({"missing.dll"})); fails([&] { copy_runtime(tmp.path, {file}, {}); }, "missing runtime"); });
    test("SHA256 streaming", [] { Temporary tmp; write(tmp.path / "abc", "abc"); check(file_sha256(tmp.path / "abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); });
    test("cache preserves flag spelling", [] { Temporary tmp; write(tmp.path / "CMakeCache.txt", "# comment\r\nCMAKE_CXX_FLAGS:STRING=/O3 /QxHost\r\n"); check(cache_values(tmp.path).at("CMAKE_CXX_FLAGS") == "/O3 /QxHost"); });
    test("unmanaged output preserved", [] {
        Temporary tmp; auto stage = tmp.path / "stage", out = tmp.path / "host11"; fs::create_directory(stage); fs::create_directory(out); write(out / "keep", "original");
        fails([&] { publish(stage, out); }, "not created"); check(fs::is_regular_file(out / "keep"));
    });
    test("backup and isolated publication", [] {
        Temporary tmp; auto stage = tmp.path / "stage", out = tmp.path / "host11", other = tmp.path / "host16";
        fs::create_directory(stage); fs::create_directory(out); fs::create_directory(other);
        atomic_json(out / "CLUSTER_BUILD.json", Json{{"node", "host11"}}); write(out / "old", "old"); write(stage / "new", "new"); write(other / "keep", "keep");
        auto backup = utf8_path(publish(stage, out)); check(fs::exists(backup / "old") && fs::exists(out / "new") && fs::exists(other / "keep"));
    });
    test("publication failure restores previous", [] {
        Temporary tmp; auto out = tmp.path / "host11"; fs::create_directory(out); atomic_json(out / "CLUSTER_BUILD.json", Json{{"node", "host11"}}); write(out / "old", "old");
        fails([&] { publish(tmp.path / "missing-stage", out); }); check(fs::exists(out / "old"));
    });
    test("atomic JSON replacement", [] { Temporary tmp; auto file = tmp.path / "summary.json"; atomic_json(file, Json{{"a", 1}}); atomic_json(file, Json{{"a", 2}}); check(strata::runtime::read_json(file)["a"] == 2); });
    test("duplicate metadata fields rejected", [] { Temporary tmp; write(tmp.path / "bad.json", "{\"node\":\"host11\",\"node\":\"host16\"}"); fails([&] { strata::runtime::read_json(tmp.path / "bad.json"); }); });
    test("dry run creates nothing", [] {
        Temporary tmp; auto out = tmp.path / "out", build = tmp.path / "build";
        const auto args = std::vector<std::string>{"builder", "--dry-run", "--build-root", path_utf8(build), "--out-root", path_utf8(out)};
        check(execute(parse(args)) == 0); check(!fs::exists(out) && !fs::exists(build));
    });
    test("overlaps and duplicate nodes rejected", [] {
        Temporary tmp;
        fails([&] { parse({"builder", "--dry-run", "--hosts", "host11", "--build-root", path_utf8(tmp.path), "--out-root", path_utf8(tmp.path)}); }, "independent");
        fails([&] { parse({"builder", "--hosts", "host11", "host11"}); }, "distinct");
        fails([&] { parse({"builder", "--jobs", "6junk"}); });
        fails([&] { parse({"builder", "--hip-archs", "gfx1102", "gfx1102"}); }, "distinct");
    });
    test("package binary and Python limits", [] {
        Temporary tmp;
#ifdef _WIN32
        const std::vector<std::string> names{"strata-host.exe", "strata_network_backend.dll"};
#else
        const std::vector<std::string> names{"strata-host", "libstrata_network_backend.so"};
#endif
        for (const auto& name : names) write(tmp.path / name);
        verify_package(tmp.path, names);
        for (const auto& name : {"strata-server.exe", "test.py", "test.pyw", "test.pyc", "module.pyd", "python3.dll", "strata-extra.dll"}) {
            write(tmp.path / name); fails([&] { verify_package(tmp.path, names); }); fs::remove(tmp.path / name);
        }
        fs::create_directory(tmp.path / "nested"); write(tmp.path / "nested" / names.back());
        fails([&] { verify_package(tmp.path, names); }, "nested project");
    });
    test("contained native child and log", [] {
        Temporary tmp; const auto before = fs::current_path();
        const auto log = tmp.path / "child.log";
        check(command({path_utf8(strata::runtime::executable_path()), "--child"}, tmp.path, log) == 7);
        check(fs::current_path() == before); std::ifstream in(log); std::string text((std::istreambuf_iterator<char>(in)), {}); check(text.find("native child output") != std::string::npos);
    });
#ifdef _WIN32
    test("complete package manifest and replacement", [] {
        PackageFixture fixture; const auto& options = fixture.options; const auto out = options.out_root / "host11";
        check(execute(options) == 0);
        auto meta = strata::runtime::read_json(out / "CLUSTER_BUILD.json");
        check(meta["programs"]["strata-host.exe"] == file_sha256(options.builds.at("host11") / "strata-host.exe"));
        check(meta["files"]["web/index.html"] == file_sha256(out / "web/index.html"));
        check(execute(options) == 0);
        const auto summary = strata::runtime::read_json(options.out_root / "build-summary.json");
        auto backup = utf8_path(summary["host11"]["previous_output"].get<std::string>());
        check(fs::is_regular_file(backup / "strata-host.exe"));
    });
    test("failed package preserves published output", [] {
        PackageFixture fixture; const auto& options = fixture.options; const auto out = options.out_root / "host11";
        check(execute(options) == 0); const auto hash = file_sha256(out / "CLUSTER_BUILD.json");
        binary(options.builds.at("host11") / "strata-host.exe", pe({"missing.dll"}));
        check(execute(options) == 1); check(file_sha256(out / "CLUSTER_BUILD.json") == hash);
        check(strata::runtime::read_json(options.out_root / "build-summary.json")["host11"]["status"] == "failed");
        for (const auto& entry : fs::directory_iterator(options.out_root)) check(!path_utf8(entry.path().filename()).starts_with(".cluster-stage-"));
    });
    test("node lock refuses concurrent publication", [] {
        PackageFixture fixture; const auto& options = fixture.options;
        fs::create_directories(options.out_root / ".cluster-lock-host11");
        check(execute(options) == 1); check(!fs::exists(options.out_root / "host11"));
        check(fs::is_directory(options.out_root / ".cluster-lock-host11"));
    });
    test("cancelled build does not start or publish later nodes", [] {
        PackageFixture fixture; auto& options = fixture.options;
        options.package_only = false; options.hosts.push_back("host17"); options.builds["host17"] = fixture.temporary.path / "other-build";
        fs::create_directory(options.root / "tools"); write(options.root / "tools/build_cluster_intel.bat", "@echo off\r\nexit /b 130\r\n");
        check(execute(options) == 130);
        auto summary = strata::runtime::read_json(options.out_root / "build-summary.json");
        check(summary["host11"]["status"] == "cancelled" && summary["host17"]["status"] == "cancelled");
        check(!fs::exists(options.out_root / "host11") && !fs::exists(options.builds.at("host17")));
    });
    test("batch entry quoting Unicode and spaces", [] {
        Temporary tmp; auto script = tmp.path / utf8_path("构建 with spaces.cmd");
        write(script, "@echo off\r\necho native batch output %1\r\nexit /b 7\r\n");
        auto cmd = utf8_path(strata::runtime::environment_utf8("SystemRoot")) / "System32/cmd.exe";
        auto log = tmp.path / "batch.log";
        check(command({path_utf8(cmd), "/d", "/c", "call", path_utf8(script), "host11"}, tmp.path, log) == 7);
        std::ifstream in(log); std::string text((std::istreambuf_iterator<char>(in)), {}); check(text.find("native batch output host11") != std::string::npos);
    });
#endif
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
