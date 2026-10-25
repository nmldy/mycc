// mycc.cpp — 极简多文件 C/C++ 构建驱动
// 平台: 仅 POSIX (Android NDK / Linux)。Windows 下 cmd.exe 的参数解析规则
//       与本工具的引号转义不兼容, 不做支持。
//
// Android 编译:
//   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
//     --target=aarch64-linux-android26 -std=c++17 -O2 \
//     -static-libstdc++ mycc.cpp -o mycc
// 桌面 Linux 编译:
//   g++ -std=c++17 -O2 mycc.cpp -o mycc
//
// 用法 (两词起步):
//   ./mycc main.cpp            # 单文件 -> 可执行 ./main
//   ./mycc src/                # 目录递归收集源码 -> ./a.out
//   ./mycc a.cpp b.cpp -o app  # 多文件请用 -o
//   ./mycc src/ -O2 -lpthread  # -O/-W/-l 等直接写, 原样传给编译器
//   ./mycc src/ -c             # 只编译, .o 在 .mycc/ 下
//   ./mycc main.cpp libext.a -o app   # .o/.a 直接进链接, 不参与编译
//   ./mycc src/ --no-cache     # 关闭增量, 强制全量重编
//
// ─── ARCHIVED NOTES: 已登记但未实现 ───────────────────────────────────────
// 以下事项经过评审, 有意暂缓。每条附"为什么没做"与"何时该做"。
//
// 1. 多个 .a 相互依赖的链接
//    现状: build_link_cmd 按扩展名分组 (.o 在前 .a 在后), 单向依赖可正常链接。
//    没做: -Wl,--start-group ... -Wl,--end-group 会禁用链接器单遍解析优化,
//          全量加上有性能代价; 按需手动加进 extra 即可绕过。
//    何时做: 出现真实的循环依赖 .a 组合, 且用户不愿手写 start-group 时。
//
// 2. -Xlinker <arg> 成对参数
//    现状: is_link_only_arg 只识别 "-Wl," 前缀; "-Xlinker foo" 作为独立
//          参数会被拆进编译命令 (foo 甚至可能被当成非选项参数丢弃)。
//    没做: 需要把 build_compile_cmd 改成参数向量感知的解析 (识别成对形式),
//          现有"逐字符串过滤"模型表达不了。
//    何时做: 用户报 "-Xlinker 在编译期报错" 时, 顺带把整套参数传递
//          重构为 argv 数组 + posix_spawn (见第 5 条), 一次收掉。
//
// 3. 头文件依赖追踪 (增量编译已知局限)
//    现状: 只比源文件 mtime, 改 .h 不触发重编; usage Notes 已写明,
//          用户可用 --no-cache 全量兜底。
//    路线: 中等方案 = 解析 #include "..." 递归收集依赖 (纯 C++ 零依赖);
//          正统方案 = -MD -MF <obj>.d + Makefile 风格 depfile 解析。
//    何时做: 有真实用户被坑后, 优先上 -MF 方案 (clang 原生支持, 但需
//          先在 Android 实机确认 depfile 行为, 见 C 阶段联调)。
//
// 4. mtime 相等语义 (增量判定已知边界)
//    现状: needs_compile 里 obj_t < src_t 才重编, 等号归命中侧 (激进语义)。
//    没做: ext4/f2fs 纳秒 mtime 下同刻度碰撞趋零; 改保守 (等号也重编)
//          会让"编完立即重跑"场景负优化。
//    何时做: 出现"改了源码但没重编"且确认是同刻度碰撞的实测报告时。
//
// 5. posix_spawn 替换 std::system
//    现状: 经 /bin/sh -c, 受 shell 可用性/SELinux 域影响; run_cmd 已在
//          system() 返回 -1 时打 errno 提示。
//    没做: 需要把字符串拼接改为 argv 数组, shq() 整体退役, 波及全部
//          命令构造函数。
//    何时做: 与第 2 条合并处理; 或 Android 实机确认 /bin/sh 不可用时。
//
// 6. CLI 简化 (已落地, 留档决策依据)
//    移除: --use-cache (缓存默认开启, 失去存在必要), --keep-objs (不再
//          自动清理任何目录), 临时 obj 目录逻辑 (obj_dir 恒为持久目录)。
//    变更: 默认输出名派生 (单源文件 -> 同名可执行); -c 的 .o 从当前目录
//          移入 .mycc/; .mycc/ 为纯缓存, 任意时刻 rm -rf 安全。
//    依据: 用户反馈"调用太长"; 高频路径收敛为 mycc <src> 两个词,
//          复杂度来源 (临时目录/清理时机/缓存开关组合) 一并消除。
//
// 相关验证材料: tests/smoke.sh (Linux 冒烟), tests/ANDROID-CHECKLIST.md (实机联调)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>  // POSIX: 解码 system() 的 wait status

namespace fs = std::filesystem;

struct Options {
    std::vector<std::string> inputs;
    std::string output     = "a.out";
    bool output_set        = false;         // 用户是否显式传了 -o
    std::string std_flag;                       // 完整形式: -std=c++17
    std::vector<std::string> includes;          // -I...
    std::vector<std::string> libdirs;           // -L...
    std::vector<std::string> libs;              // -l...
    std::vector<std::string> defines;           // -D...
    std::vector<std::string> extra;             // 其它参数原样传给 clang
    std::string cc  = "clang";
    std::string cxx = "clang++";
    std::string obj_dir = ".mycc";              // 中间产物目录, 恒为持久目录
    unsigned jobs     = 0;                      // 0 = 未指定 (运行时取 CPU 核数)
    bool only_compile = false;
    bool verbose      = false;
    bool cc_explicit  = false;                  // 用户是否显式传了 --cc
    bool no_cache     = false;                  // --no-cache: 强制全量重编
};

static void die(const std::string& msg) {
    std::cerr << "mycc: error: " << msg << "\n";
    std::exit(1);
}

static void usage() {
    std::cout <<
R"(mycc — 极简 C/C++ 构建驱动

快速上手:
  mycc main.cpp            单文件 -> 可执行 ./main
  mycc src/                目录递归收集源码 -> ./a.out
  mycc a.cpp b.cpp -o app  多文件请用 -o
  mycc src/ -O2 -lpthread  -O/-W/-l 等直接写, 原样传给编译器

选项:
  -o <path>        输出路径 (默认: 单个源文件 -> 同名, 其余 -> a.out)
  -c               只编译: .o 生成在 .mycc/ 下, 与源文件同名
  -std=<std>       语言标准, 如 -std=c++17
  -I<dir> -L<dir> -l<name> -D<macro>[=v]   同 cc 惯例 (空格分开写也行)
  -j<N>            并行数 (默认 CPU 核数)
  --no-cache       关闭增量编译, 强制全量重编
  --obj-dir <dir>  中间产物目录 (默认 .mycc)
  --cc <path>      C 编译器 (默认 clang)
  --cxx <path>     C++ 编译器 (默认 clang++)
  -v, --verbose    显示执行的命令与缓存命中
  -h, --help       帮助

增量缓存 (默认开启):
  判定: .o 存在 且 .o mtime >= 源 mtime 且 编译参数指纹一致, 缺一重编。
  指纹 = FNV-1a(剔除 -o 路径后的编译命令), 存为 <obj>.myccfp;
  指纹缺失/损坏 -> 一律重编 (fail-safe)。
  .mycc/ 是纯缓存: 任何时候 rm -rf .mycc 都安全, 下次自动重建。
  已知局限: 不追踪头文件依赖, 改 .h 不会触发重编, 需要时 --no-cache 全量;
  mv 源目录会使指纹失效触发全量 (预期行为, 路径是编译语义的一部分)。
  目录扫描不跟随符号链接; -Wl,... 只传链接不传编译。
  全 .c 项目用 cc 链接不引入 C++ 运行时; 纯链接默认 cxx, 显式 --cc 则尊重。
  .o/.a 输入直接进链接, 不参与编译; 静态库自动排在所有目标文件之后。
  -c 至少要有一个源文件。
)";
}

static bool is_c_source(const fs::path& p) {
    return p.extension() == ".c";
}
static bool is_cxx_source(const fs::path& p) {
    auto e = p.extension().string();
    return e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".c++" || e == ".C";
}
static bool is_link_object(const fs::path& p) {
    auto e = p.extension().string();
    return e == ".o" || e == ".a";
}

static unsigned parse_jobs(const std::string& value) {
    try {
        size_t pos = 0;
        unsigned long v = std::stoul(value, &pos);
        if (pos != value.size())
            die("invalid value for -j: " + value);
        if (v == 0)
            die("-j value must be >= 1 (got 0)");
        return (unsigned)v;
    } catch (const std::exception&) {
        die("invalid value for -j: " + value);
    }
    return 0; // unreachable, silence warnings
}

// 执行 shell 命令, 将 system() 的 wait status 正确解码为退出码
// 语义不变: 返回 0 表示成功, 非 0 表示失败
static int run_cmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    if (rc == -1) {
        std::fprintf(stderr, "mycc: system() failed: %s "
                             "(check /bin/sh availability)\n", std::strerror(errno));
        return -1;
    }
    if (WIFEXITED(rc))   return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return rc;
}

static Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) die(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "-h" || a == "--help")            { usage(); std::exit(0); }
        // 大括号不可省: 此处曾因缺大括号导致 cc_explicit 恒真
        else if (a == "--cc")                      { o.cc = need("--cc"); o.cc_explicit = true; }
        else if (a == "--cxx")                      o.cxx = need("--cxx");
        else if (a == "--obj-dir")                  o.obj_dir = need("--obj-dir");
        else if (a == "-c")                         o.only_compile = true;
        else if (a == "-v" || a == "--verbose")     o.verbose = true;
        else if (a == "--no-cache")                 o.no_cache = true;
        else if (a == "-o")                        { o.output = need("-o"); o.output_set = true; }
        else if (a.rfind("-o", 0) == 0 && a.size() > 2) { o.output = a.substr(2); o.output_set = true; }
        else if (a == "-std")                       o.std_flag = "-std=" + need("-std");
        else if (a.rfind("-std=", 0) == 0)          o.std_flag = a;
        else if (a == "-I")                         o.includes.push_back(need("-I"));
        else if (a.rfind("-I", 0) == 0)             o.includes.push_back(a.substr(2));
        else if (a == "-L")                         o.libdirs.push_back(need("-L"));
        else if (a.rfind("-L", 0) == 0)             o.libdirs.push_back(a.substr(2));
        else if (a == "-l")                         o.libs.push_back(need("-l"));
        else if (a.rfind("-l", 0) == 0)             o.libs.push_back(a.substr(2));
        else if (a == "-D")                         o.defines.push_back(need("-D"));
        else if (a.rfind("-D", 0) == 0)             o.defines.push_back(a.substr(2));
        else if (a == "-j")                         o.jobs = parse_jobs(need("-j"));
        else if (a.rfind("-j", 0) == 0 && a.size() > 2)
                                                    o.jobs = parse_jobs(a.substr(2));
        else if (!a.empty() && a[0] == '-')         o.extra.push_back(a);
        else                                        o.inputs.push_back(a);
    }
    if (o.inputs.empty()) { usage(); die("no input files"); }

    // [新增] 默认输出名派生: 未显式 -o 且恰好一个输入且是源文件 -> 同名。
    // 目录输入不派生: mycc src 时 cwd 里有 src/ 目录, 同名可执行必然冲突。
    if (!o.output_set && o.inputs.size() == 1) {
        fs::path p(o.inputs[0]);
        if ((is_c_source(p) || is_cxx_source(p)) && fs::is_regular_file(p))
            o.output = p.stem().string();
    }
    return o;
}

// 注: 递归遍历不跟随目录符号链接, 避免循环链接; 额外路径请用 -I 显式指定
// 单文件分支: 源码进 sources, .o/.a 进 link_inputs, 其余明确报错
static void collect_sources(const std::vector<std::string>& inputs,
                            std::vector<fs::path>& sources,
                            std::vector<fs::path>& link_inputs) {
    for (auto& s : inputs) {
        fs::path p(s);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            fs::recursive_directory_iterator it(
                p, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            while (!ec && it != end) {
                std::error_code entry_ec;               // 局部 ec, 不污染遍历状态
                if (it->is_regular_file(entry_ec)) {
                    const fs::path& fp = it->path();
                    if (is_c_source(fp) || is_cxx_source(fp))
                        sources.push_back(fp);
                    // 目录扫描只收源码, 不收散落的 .o/.a (避免把构建产物重复链入)
                }
                it.increment(ec);
            }
            if (ec) die("failed to iterate directory " + s + ": " + ec.message());
        } else if (fs::is_regular_file(p, ec)) {
            if (is_c_source(p) || is_cxx_source(p)) {
                sources.push_back(p);
            } else if (is_link_object(p)) {
                link_inputs.push_back(p);
            } else {
                die("unsupported input file: " + s +
                    " (expected .c/.cc/.cpp/.cxx/.c++/.C source, or .o/.a for linking)");
            }
        } else {
            die("input not found: " + s);
        }
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    std::sort(link_inputs.begin(), link_inputs.end());
    link_inputs.erase(std::unique(link_inputs.begin(), link_inputs.end()), link_inputs.end());
}

static std::string shq(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else           r += c;
    }
    r += "'";
    return r;
}

static uint64_t fnv1a(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static std::string hash_to_hex(uint64_t h) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

static std::string obj_name_for(const fs::path& src, bool simple) {
    std::string base = src.stem().string();
    for (auto& c : base)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    if (simple) return base + ".o";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)fnv1a(src.string()));
    return base + "_" + buf + ".o";
}

// 指纹文件读写: <obj>.myccfp, 内容为 16 位十六进制哈希。
// 读失败/内容异常一律视为"指纹不匹配" -> 重编 (fail-safe)
static fs::path fingerprint_path_for(const fs::path& obj) {
    return obj.string() + ".myccfp";
}
static bool read_fingerprint(const fs::path& fp, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(fp, ec)) return false;
    std::FILE* f = std::fopen(fp.string().c_str(), "rb");
    if (!f) return false;
    char buf[128] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    size_t b = 0, e = n;
    while (b < e && std::isspace((unsigned char)buf[b])) ++b;
    while (e > b && std::isspace((unsigned char)buf[e - 1])) --e;
    out.assign(buf + b, e - b);
    return true;
}
static bool write_fingerprint(const fs::path& fp, const std::string& hash) {
    std::FILE* f = std::fopen(fp.string().c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(hash.data(), 1, hash.size(), f) == hash.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

// 增量判定: .o 存在 且 指纹一致 且 .o mtime >= 源 mtime; 任一不满足即重编
static bool needs_compile(const Options& o, const fs::path& src, const fs::path& obj,
                          const std::string& fp_hash) {
    if (o.no_cache) return true;
    std::error_code ec;
    if (!fs::is_regular_file(obj, ec)) return true;

    std::string stored;
    if (!read_fingerprint(fingerprint_path_for(obj), stored) || stored != fp_hash)
        return true;

    auto src_t = fs::last_write_time(src, ec);
    if (ec) return true;                             // 取不到 mtime -> 重编 (fail-safe)
    auto obj_t = fs::last_write_time(obj, ec);
    if (ec) return true;
    // 已知边界: obj_t == src_t 归命中侧 (激进语义, 少编)。
    // ext4/f2fs 等纳秒 mtime 下同刻度碰撞概率趋近零; 即使触发, 后果是本次
    // 少编一次且下次运行必然修复。若需对齐 make 的保守语义 (等号重编),
    // 改为: if (!(obj_t > src_t)) return true; 代价是快速迭代场景的负优化。
    if (obj_t < src_t) return true;

    if (o.verbose) std::cerr << "mycc: cache hit: " << src.string() << "\n";
    return false;
}

// 编译期跳过仅链接有意义的参数 (-Wl,...)
// TODO: 后续如需支持 -Xlinker <arg> 成对形式, 需改用参数向量拼接
static bool is_link_only_arg(const std::string& a) {
    return a.rfind("-Wl,", 0) == 0;
}

// C 文件编译时过滤掉任何 C++ 语言标准 (-std=c++XX / -std=gnu++XX /
// 未来 c++2b 等), 用 "++" 子串判定, 覆盖所有 C++ 标准命名变体
static bool is_cxx_std_flag(const std::string& flag) {
    return flag.rfind("-std=", 0) == 0 && flag.find("++") != std::string::npos;
}

// [注意] 签名: 第 4 参数 fingerprint_cmd_out。
//   非 nullptr 时, 输出"剔除 -o <obj> 后的完整编译命令"用于缓存指纹,
//   使 --obj-dir 改名不会作废全部缓存。nullptr 表示不需要指纹。
//   (主线程计算指纹时传有效指针; worker 执行编译时传 nullptr, 避免重复拼装)
// 实现上先拼 head(-c src) 与 tail(所有 flags), 再组合:
//   完整命令 = head + tail + " -o obj"
//   指纹命令 = head + tail (不含 -o 路径)
static std::string build_compile_cmd(const Options& o, const fs::path& src, const fs::path& obj,
                                     std::string* fingerprint_cmd_out) {
    const bool is_c = is_c_source(src);
    std::string cmd = is_c ? o.cc : o.cxx;
    cmd += " -c " + shq(src.string());
    std::string fp_cmd = cmd;                        // 指纹命令起点 (不含 -o)

    std::string stdflag = o.std_flag;
    if (is_c && is_cxx_std_flag(stdflag)) stdflag.clear();
    if (!stdflag.empty()) { cmd += " " + shq(stdflag); fp_cmd += " " + shq(stdflag); }

    for (auto& d : o.defines) {
        std::string piece = " -D" + shq(d);
        cmd += piece; fp_cmd += piece;
    }
    for (auto& i : o.includes) {
        std::string piece = " -I" + shq(i);
        cmd += piece; fp_cmd += piece;
    }
    for (auto& e : o.extra) {
        if (is_link_only_arg(e)) continue;
        std::string piece = " " + shq(e);
        cmd += piece; fp_cmd += piece;
    }

    cmd += " -o " + shq(obj.string());               // 仅完整命令含 -o, 且在 flags 之后
    if (fingerprint_cmd_out) *fingerprint_cmd_out = fp_cmd;
    return cmd;
}

// [注意] 签名: (Options, objs, link_inputs, all_c)。
// 输出顺序: 编译产物 -> 用户 .o -> 用户 .a。链接器从左到右单向解析符号,
// 静态库 (.a) 必须位于所有引用它的目标文件之后; collect_sources 的字典序
// 排序会把 libext.a 排到 main.o 之前, 因此这里按扩展名二次分组。
// all_c: 全部源文件为 .c (或用户显式 --cc 的纯链接) 时用 C 编译器链接。
// TODO: 多个 .a 相互依赖时需 -Wl,--start-group ... -Wl,--end-group, 遇到再加
static std::string build_link_cmd(const Options& o, const std::vector<fs::path>& objs,
                                  const std::vector<fs::path>& link_inputs, bool all_c) {
    std::string cmd = all_c ? o.cc : o.cxx;
    for (auto& ob : objs)        cmd += " " + shq(ob.string());        // 编译产物
    for (auto& li : link_inputs)                                        // 用户 .o 在前
        if (li.extension() != ".a") cmd += " " + shq(li.string());
    for (auto& li : link_inputs)                                        // .a 全部在后
        if (li.extension() == ".a") cmd += " " + shq(li.string());
    cmd += " -o " + shq(o.output);
    for (auto& l : o.libdirs) cmd += " -L" + shq(l);
    for (auto& l : o.libs)    cmd += " -l" + shq(l);
    for (auto& e : o.extra)   cmd += " " + shq(e);
    return cmd;
}

// 中间产物目录: 默认 .mycc (cwd 下), --obj-dir 可覆盖。
// 恒为持久目录, 从不自动清理 (rm -rf .mycc 完全安全);
// 临时目录逻辑已随 CLI 简化移除 (见头部 ARCHIVED NOTES 第 6 条)。
static fs::path make_obj_dir(const Options& o) {
    std::error_code ec;
    fs::create_directories(o.obj_dir, ec);
    if (ec) die("cannot create obj dir " + o.obj_dir + ": " + ec.message());
    return o.obj_dir;
}

int main(int argc, char** argv) {
    Options o = parse_args(argc, argv);

    std::vector<fs::path> sources, link_inputs;
    collect_sources(o.inputs, sources, link_inputs);
    if (sources.empty() && link_inputs.empty()) die("no source files found");

    // [新增] 输出路径是目录 -> 提前报错 (否则 clang 的模糊报错很难懂)。
    // 派生名也可能撞上同名目录 (如 cwd 里有 main/ 目录且源文件叫 main.cpp)
    if (!o.only_compile) {
        std::error_code ec;
        if (fs::is_directory(o.output, ec))
            die("output path is a directory: " + o.output + " (use -o to pick another name)");
    }

    if (o.only_compile && o.output_set)
        std::cerr << "mycc: warning: -o is ignored in -c (compile-only) mode\n";
    if (o.only_compile && sources.empty())
        die("-c (compile-only) requires at least one source file; "
            ".o/.a inputs cannot be compiled");

    if (o.verbose) {
        std::cerr << "sources (" << sources.size() << "):\n";
        for (auto& s : sources) std::cerr << "  " << s.string() << "\n";
        if (!link_inputs.empty()) {
            std::cerr << "link inputs (" << link_inputs.size() << "):\n";
            for (auto& s : link_inputs) std::cerr << "  " << s.string() << "\n";
        }
        std::cerr << "cache: " << (o.no_cache ? "off (--no-cache)" : "on") << "\n";
    }

    // 链接器选择:
    //   有源文件   -> 全 .c 用 cc, 否则 cxx
    //   纯链接     -> 默认 cxx (语言未知的安全超集); 用户显式传了 --cc 则尊重
    bool all_c;
    if (sources.empty()) {
        all_c = o.cc_explicit;
    } else {
        all_c = true;
        for (auto& s : sources)
            if (!is_c_source(s)) { all_c = false; break; }
    }

    if (o.jobs == 0) o.jobs = std::max(1u, std::thread::hardware_concurrency());

    struct Job {
        fs::path    src;
        fs::path    obj;
        bool        needs;      // 是否需要编译 (缓存命中为 false)
        std::string fp_hash;    // 编译参数指纹 (主线程算好, worker 直接用)
    };
    std::vector<Job> jobs;
    fs::path obj_dir;
    size_t cache_hits = 0;

    obj_dir = make_obj_dir(o);

    // -c 模式 .o 名冲突检测: 冲突时回退为带哈希的唯一名
    std::set<std::string> used_obj_names;
    jobs.reserve(sources.size());
    for (auto& s : sources) {
        std::string nm = obj_name_for(s, o.only_compile);
        if (!used_obj_names.insert(nm).second) {
            nm = obj_name_for(s, false);
            while (!used_obj_names.insert(nm).second) nm = "dup_" + nm;
        }
        fs::path obj = obj_dir / nm;

        // 计算指纹并判定是否需要编译 (主线程串行, 纯 fs 操作)
        std::string fp_cmd;
        build_compile_cmd(o, s, obj, &fp_cmd);
        std::string fp_hash = hash_to_hex(fnv1a(fp_cmd));
        bool need = needs_compile(o, s, obj, fp_hash);
        if (!need) ++cache_hits;
        jobs.push_back({s, obj, need, fp_hash});
    }
    if (o.verbose)
        std::cerr << "cache: " << cache_hits << " hit(s), "
                  << (jobs.size() - cache_hits) << " to compile\n";

    std::vector<int> results(jobs.size(), 0);
    std::atomic<size_t> next{0};
    std::mutex io_mu;

    auto worker = [&]() {
        while (true) {
            size_t i = next.fetch_add(1);
            if (i >= jobs.size()) break;
            if (!jobs[i].needs) { results[i] = 0; continue; }   // 缓存命中跳过

            // 指纹已在主线程算好存于 jobs[i].fp_hash, 此处传 nullptr
            std::string cmd = build_compile_cmd(o, jobs[i].src, jobs[i].obj, nullptr);
            if (o.verbose) {
                std::lock_guard<std::mutex> lk(io_mu);
                std::cerr << "+ " << cmd << "\n";
            }
            int rc = run_cmd(cmd);
            results[i] = (rc == 0) ? 0 : 1;

            // 编译成功后写指纹文件; 写失败仅警告, 下次重编 (fail-safe)。
            // lock_guard 与 cerr 必须在同一作用域块 (此前缺大括号导致编译错误)
            if (rc == 0 && !o.no_cache) {
                if (!write_fingerprint(fingerprint_path_for(jobs[i].obj), jobs[i].fp_hash)) {
                    std::lock_guard<std::mutex> lk(io_mu);
                    std::cerr << "mycc: warning: cannot write fingerprint for "
                              << jobs[i].obj.string() << " (will recompile next run)\n";
                }
            }
        }
    };

    unsigned n = std::min<unsigned>(o.jobs, (unsigned)jobs.size());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < n; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    size_t failed = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        if (results[i]) {
            std::cerr << "mycc: failed: " << jobs[i].src.string() << "\n";
            ++failed;
        }
    }
    if (failed) {
        // 不清理: .mycc 是纯缓存, 已成功的 .o 下次直接命中, 修复后只重编坏的
        die(std::to_string(failed) + " file(s) failed");
    }

    int rc = 0;
    if (o.only_compile) {
        std::cout << "compiled " << (jobs.size() - cache_hits)
                  << " object(s) (" << cache_hits << " cached) into "
                  << obj_dir.string() << "\n";
    } else {
        std::vector<fs::path> objs;
        for (auto& j : jobs) objs.push_back(j.obj);
        std::string cmd = build_link_cmd(o, objs, link_inputs, all_c);
        if (o.verbose) std::cerr << "+ " << cmd << "\n";
        rc = run_cmd(cmd);
        if (rc == 0) std::cout << "built " << o.output << "\n";
        else         std::cerr << "mycc: link failed\n";
        // 不清理: .mycc 持久保留, 供下次增量复用
    }
    return rc == 0 ? 0 : 1;
}
