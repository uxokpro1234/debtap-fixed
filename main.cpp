//MAYBE YOU NEED THIS

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path deb;
    fs::path output = fs::current_path();
    std::optional<fs::path> mapFile;
    bool writePkgbuild = false;
    bool pkgbuildOnly = false;
    bool keepWork = false;
    bool runNamcap = false;
    bool force = false;
    bool pseudo = false;
    bool wipeout = false;
    bool quiet = false;
    bool quietAll = false;
};

struct CommandResult {
    int status = -1;
};

class TempDirectory {
public:
    explicit TempDirectory(bool keep) : keep_(keep) {
        std::string pattern = (fs::temp_directory_path() / "debtap-cpp-XXXXXX").string();
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        char* result = ::mkdtemp(buf.data());
        if (!result) throw std::runtime_error("mkdtemp failed: " + std::string(std::strerror(errno)));
        path_ = result;
    }
    ~TempDirectory() {
        if (!keep_) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        } else {
            std::cerr << "[debtap-cpp] Kept work directory: " << path_ << "\n";
        }
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
    bool keep_;
};

std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(s.begin(), s.end(), notSpace);
    auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    if (b >= e) return {};
    return std::string(b, e);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

int normalizeStatus(int status) {
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

// Run an external command without a shell. This keeps file names from becoming shell code.
CommandResult runCommand(const std::vector<std::string>& args,
                         const std::optional<fs::path>& cwd = std::nullopt,
                         const std::optional<fs::path>& stdoutFile = std::nullopt,
                         bool quietStdout = false) {
    if (args.empty()) throw std::runtime_error("internal error: empty command");
    pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    if (pid == 0) {
        if (cwd && ::chdir(cwd->c_str()) != 0) {
            std::cerr << "chdir " << cwd->string() << ": " << std::strerror(errno) << "\n";
            _exit(126);
        }
        if (stdoutFile) {
            int fd = ::open(stdoutFile->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) {
                std::cerr << "redirect stdout: " << std::strerror(errno) << "\n";
                _exit(126);
            }
            ::close(fd);
        } else if (quietStdout) {
            int fd = ::open("/dev/null", O_WRONLY);
            if (fd >= 0) { ::dup2(fd, STDOUT_FILENO); ::close(fd); }
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::cerr << "exec " << args[0] << ": " << std::strerror(errno) << "\n";
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
    }
    return {normalizeStatus(status)};
}

void requireCommand(const std::string& name) {
    // `command -v` is a shell builtin, so inspect PATH directly instead.
    const char* pathEnv = std::getenv("PATH");
    std::string path = pathEnv ? pathEnv : "/usr/bin:/bin";
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        fs::path candidate = fs::path(dir) / name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) return;
    }
    throw std::runtime_error("required command not found in PATH: " + name);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read file: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& contents, bool executable = false) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write file: " + p.string());
    out << contents;
    out.close();
    if (!out) throw std::runtime_error("error writing file: " + p.string());
    if (executable) {
        std::error_code ec;
        fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
    }
}

std::string safePkgName(std::string s) {
    s = lower(trim(s));
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::islower(u) || std::isdigit(u) || c == '@' || c == '.' || c == '_' || c == '+' || c == '-')) c = '-';
    }
    while (!s.empty() && (s.front() == '-' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '-' || s.back() == '.')) s.pop_back();
    if (s.empty()) throw std::runtime_error("package name is empty after Arch name normalization");
    return s;
}

std::string safeVersion(std::string s) {
    s = trim(s);
    // Convert Debian epoch syntax into an explicit sortable component in pkgver.
    s = replaceAll(s, ":", "_");
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        // Arch pkgver values may not contain '-' because the final '-' separates pkgrel.
        if (c == '-') c = '_';
        else if (!(std::isalnum(u) || c == '.' || c == '_' || c == '+')) c = '_';
    }
    while (!s.empty() && (s.front() == '-' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '-' || s.back() == '.')) s.pop_back();
    if (s.empty()) s = "0";
    return s;
}

std::string safeText(std::string s) {
    s = trim(s);
    s = replaceAll(s, "\r", "");
    s = replaceAll(s, "\n", " ");
    return s;
}

std::string shellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string urlEncodePath(const fs::path& p) {
    const std::string in = fs::absolute(p).string();
    std::ostringstream out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << hex[c >> 4] << hex[c & 0x0f];
        }
    }
    return "file://" + out.str();
}

std::map<std::string, std::string> loadNameMap(const std::optional<fs::path>& mapFile) {
    std::map<std::string, std::string> result;
    std::vector<fs::path> files;
    if (mapFile) files.push_back(*mapFile);
    else {
        if (fs::exists("/etc/debtap/name-map")) files.emplace_back("/etc/debtap/name-map");
        if (const char* home = std::getenv("HOME")) {
            fs::path p = fs::path(home) / ".config/debtap/name-map";
            if (fs::exists(p)) files.push_back(p);
        }
    }
    for (const auto& file : files) {
        std::ifstream in(file);
        if (!in) throw std::runtime_error("cannot open package-name map: " + file.string());
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto sep = line.find('=');
            if (sep == std::string::npos) sep = line.find_first_of(" \t");
            if (sep == std::string::npos) continue;
            std::string from = trim(line.substr(0, sep));
            std::string to = trim(line.substr(sep + 1));
            if (!from.empty() && !to.empty()) result[lower(from)] = safePkgName(to);
        }
    }
    return result;
}

std::string translateName(std::string name, const std::map<std::string, std::string>& mapping) {
    name = lower(trim(name));
    auto it = mapping.find(name);
    if (it != mapping.end()) return it->second;
    return safePkgName(name);
}

using Fields = std::map<std::string, std::string>;

Fields parseControl(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("Debian control file not found or unreadable: " + file.string());
    Fields fields;
    std::string current, line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break; // Only the first control paragraph is package metadata.
        if ((line[0] == ' ' || line[0] == '\t') && !current.empty()) {
            if (line == " ." || line == " .\r") fields[current] += "\n";
            else {
                if (!fields[current].empty()) fields[current] += "\n";
                fields[current] += line.substr(1);
            }
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        current = lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        auto it = fields.find(current);
        if (it == fields.end()) fields[current] = value;
        else it->second += ", " + value;
    }
    return fields;
}

std::vector<std::string> splitTopLevel(const std::string& s, char delim) {
    std::vector<std::string> out;
    int paren = 0;
    std::string item;
    for (char c : s) {
        if (c == '(') ++paren;
        if (c == ')' && paren > 0) --paren;
        if (c == delim && paren == 0) {
            auto t = trim(item);
            if (!t.empty()) out.push_back(t);
            item.clear();
        } else item += c;
    }
    auto t = trim(item);
    if (!t.empty()) out.push_back(t);
    return out;
}

struct Dep {
    std::string name;
    std::string op;
    std::string version;
};

std::optional<Dep> parseDebianDependency(std::string token,
                                         const std::map<std::string, std::string>& mapping) {
    token = trim(token);
    if (token.empty() || token.find("${") != std::string::npos) return std::nullopt;
    // Drop Debian architecture restrictions and multiarch qualifiers.
    auto bracket = token.find('[');
    if (bracket != std::string::npos) token = trim(token.substr(0, bracket));
    auto angle = token.find('<');
    (void)angle;
    std::string name;
    std::string rest;
    auto paren = token.find('(');
    if (paren == std::string::npos) {
        name = trim(token);
    } else {
        name = trim(token.substr(0, paren));
        auto end = token.find(')', paren + 1);
        if (end != std::string::npos) rest = trim(token.substr(paren + 1, end - paren - 1));
    }
    auto colon = name.find(':');
    if (colon != std::string::npos) name.erase(colon); // foo:any and foo:native -> foo
    if (name.empty()) return std::nullopt;
    Dep d;
    d.name = translateName(name, mapping);
    if (!rest.empty()) {
        std::istringstream ss(rest);
        ss >> d.op >> d.version;
        if (d.op == "<<") d.op = "<";
        else if (d.op == ">>") d.op = ">";
        if (d.op != ">=" && d.op != "<=" && d.op != ">" && d.op != "<" && d.op != "=" && d.op != "==") {
            d.op.clear(); d.version.clear();
        }
        if (d.op == "==") d.op = "=";
        d.version = safeVersion(d.version);
    }
    return d;
}

std::string depToPacman(const Dep& d) {
    if (d.op.empty()) return d.name;
    return d.name + d.op + d.version;
}

std::vector<std::string> parseDependencyList(const std::string& value,
                                              const std::map<std::string, std::string>& mapping,
                                              bool wipeout = false) {
    std::vector<std::string> result;
    for (const auto& group : splitTopLevel(value, ',')) {
        auto alternatives = splitTopLevel(group, '|');
        if (alternatives.size() > 1) {
            std::cerr << "[debtap-cpp] Warning: Debian alternative dependency '" << group
                      << "'; mapping only the first alternative. Review the result.\n";
        }
        if (alternatives.empty()) continue;
        auto dep = parseDebianDependency(alternatives.front(), mapping);
        if (dep) {
            if (wipeout) { dep->op.clear(); dep->version.clear(); }
            result.push_back(depToPacman(*dep));
        }
        else if (!trim(alternatives.front()).empty())
            std::cerr << "[debtap-cpp] Warning: skipped unrecognized dependency: " << alternatives.front() << "\n";
    }
    return result;
}

std::vector<std::string> uniqueSorted(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::string archName(std::string debArch) {
    debArch = lower(trim(debArch));
    static const std::map<std::string, std::string> maps = {
        {"all", "any"}, {"amd64", "x86_64"}, {"x86_64", "x86_64"},
        {"i386", "i686"}, {"i486", "i686"}, {"i586", "i686"}, {"i686", "i686"},
        {"arm64", "aarch64"}, {"aarch64", "aarch64"}, {"armhf", "armv7h"},
        {"armel", "arm"}, {"ppc64el", "powerpc64le"}, {"s390x", "s390x"},
        {"riscv64", "riscv64"}, {"mips64el", "mips64el"}, {"loong64", "loong64"}
    };
    auto it = maps.find(debArch);
    if (it != maps.end()) return it->second;
    if (debArch.empty()) return "any";
    std::cerr << "[debtap-cpp] Warning: unknown Debian architecture '" << debArch << "'; recording it as 'any'.\n";
    return "any";
}

std::optional<fs::path> findByFilename(const fs::path& root, const std::string& filename) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && it->path().filename() == filename) return it->path();
    }
    return std::nullopt;
}

std::vector<fs::path> findOuterArchiveMembers(const fs::path& dir, const std::string& prefix) {
    std::vector<fs::path> result;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) == 0) result.push_back(e.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Move a filesystem subtree to an Arch-style destination, merging directories.
// Symlinks are moved as symlinks, not followed.
void mergeMovePath(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if (!fs::exists(source, ec) && !fs::is_symlink(fs::symlink_status(source, ec))) return;
    const auto sourceStatus = fs::symlink_status(source, ec);
    const bool sourceIsDir = fs::is_directory(sourceStatus) && !fs::is_symlink(sourceStatus);
    if (sourceIsDir) {
        fs::create_directories(destination, ec);
        if (ec) throw std::runtime_error("cannot create directory " + destination.string() + ": " + ec.message());
        std::vector<fs::path> children;
        for (const auto& entry : fs::directory_iterator(source)) children.push_back(entry.path());
        for (const auto& child : children) mergeMovePath(child, destination / child.filename());
        fs::remove(source, ec);
        return;
    }
    // Match `mv source target-directory` behavior if the same-named destination is a directory.
    if (fs::is_directory(destination, ec) && !sourceIsDir) {
        mergeMovePath(source, destination / source.filename());
        return;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (ec) throw std::runtime_error("cannot create directory " + destination.parent_path().string() + ": " + ec.message());
    if (fs::exists(destination, ec) || fs::is_symlink(fs::symlink_status(destination, ec))) {
        fs::remove_all(destination, ec);
        if (ec) throw std::runtime_error("cannot replace " + destination.string() + ": " + ec.message());
    }
    fs::rename(source, destination, ec);
    if (ec) throw std::runtime_error("cannot move " + source.string() + " to " + destination.string() + ": " + ec.message());
}

void moveDirectoryContents(const fs::path& root, const std::string& sourceRelative,
                           const std::string& targetRelative) {
    const fs::path source = root / sourceRelative;
    std::error_code ec;
    const auto sourceStatus = fs::symlink_status(source, ec);
    if (ec || !fs::is_directory(sourceStatus) || fs::is_symlink(sourceStatus)) return;
    const fs::path target = root / targetRelative;
    fs::create_directories(target, ec);
    if (ec) throw std::runtime_error("cannot create " + target.string() + ": " + ec.message());
    std::vector<fs::path> children;
    for (const auto& e : fs::directory_iterator(source)) children.push_back(e.path());
    for (const auto& child : children) mergeMovePath(child, target / child.filename());
    fs::remove(source, ec);
}

void normalizeFilesystem(const fs::path& root, bool pseudo, const std::string& debArch) {
    // Fixing possible differences in filesystem hierarchy so that it matches Arch Linux.
    // Debian packages often use /bin, /sbin, /usr/sbin and /usr/games; Arch uses /usr/bin.
    for (const auto& dir : {"bin", "sbin", "usr/sbin", "usr/games", "usr/local/games",
                            "usr/local/bin", "usr/local/sbin"}) {
        moveDirectoryContents(root, dir, "usr/bin");
    }

    const bool is32BitDeb = lower(debArch) == "i386" || lower(debArch) == "i486" ||
                            lower(debArch) == "i586" || lower(debArch) == "i686";
    if (!pseudo) {
        moveDirectoryContents(root, "lib", "usr/lib");
        moveDirectoryContents(root, "lib64", "usr/lib");
        moveDirectoryContents(root, "usr/lib64", "usr/lib");
        if (is32BitDeb) {
            moveDirectoryContents(root, "lib32", "usr/lib");
            moveDirectoryContents(root, "usr/lib32", "usr/lib");
            moveDirectoryContents(root, "usr/local/lib32", "usr/local/lib");
        } else {
            moveDirectoryContents(root, "lib32", "usr/lib32");
        }
        moveDirectoryContents(root, "usr/local/lib64", "usr/local/lib");
    } else {
        // Compatibility-only pseudo mode: place 32-bit libraries under lib32.
        moveDirectoryContents(root, "lib", "usr/lib32");
        moveDirectoryContents(root, "lib32", "usr/lib32");
        moveDirectoryContents(root, "usr/lib", "usr/lib32");
        moveDirectoryContents(root, "usr/local/lib", "usr/local/lib32");
    }

    // Update desktop-launcher Exec paths after moving binaries into /usr/bin.
    const fs::path apps = root / "usr/share/applications";
    std::error_code ec;
    if (fs::is_directory(apps, ec)) {
        for (fs::recursive_directory_iterator it(apps, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec) || it->path().extension() != ".desktop") continue;
            std::string body;
            try { body = readFile(it->path()); } catch (...) { continue; }
            for (const std::string prefix : {"/bin/", "/sbin/", "/usr/sbin/", "/usr/games/",
                                             "/usr/local/bin/", "/usr/local/sbin/", "/usr/local/games/"}) {
                body = replaceAll(body, "Exec=" + prefix, "Exec=/usr/bin/");
            }
            writeFile(it->path(), body);
        }
    }
    // Discard now-empty usr/local directories, as the original workflow does.
    const fs::path local = root / "usr/local";
    if (fs::is_directory(local, ec) && fs::is_empty(local, ec)) fs::remove(local, ec);
}

void extractDebianContainer(const fs::path& deb, const fs::path& out) {
    fs::create_directories(out);
    // Most libarchive/bsdtar builds can read the ar container used by .deb files.
    auto r = runCommand({"bsdtar", "-xf", deb.string(), "--no-same-owner", "-C", out.string()});
    if (r.status != 0) {
        // Fallback for bsdtar builds that cannot read ar archives.
        if (runCommand({"ar", "x", deb.string()}, out).status != 0)
            throw std::runtime_error("cannot extract .deb outer ar archive (bsdtar and ar both failed)");
    }
}

void extractMember(const fs::path& archive, const fs::path& out) {
    fs::create_directories(out);
    auto r = runCommand({"bsdtar", "-xf", archive.string(), "--no-same-owner", "--no-same-permissions", "-C", out.string()});
    if (r.status != 0) throw std::runtime_error("failed extracting archive member: " + archive.string());
}

std::string makeInstallScript(const std::map<std::string, std::string>& scripts) {
    std::ostringstream out;
    out << "# Generated by debtap-cpp from Debian maintainer scripts.\n";
    out << "# Review carefully: this code may run as root during package operations.\n\n";
    auto emitRunner = [&](const std::string& scriptName) {
        auto it = scripts.find(scriptName);
        if (it == scripts.end()) return;
        std::string body = it->second;
        if (body.rfind("#!", 0) != 0) body = "#!/bin/sh\n" + body;
        std::string delimiter = "DEBTAP_CPP_END_" + std::to_string(std::hash<std::string>{}(body));
        while (body.find(delimiter) != std::string::npos) delimiter += "_X";
        const std::string fn = "_debtap_run_" + scriptName;
        out << fn << "() {\n"
            << "  local _debtap_tmp _debtap_rc\n"
            << "  _debtap_tmp=\"$(mktemp)\" || return 1\n"
            << "  cat > \"$_debtap_tmp\" <<'" << delimiter << "'\n"
            << body;
        if (!body.empty() && body.back() != '\n') out << '\n';
        out << delimiter << "\n"
            << "  chmod 700 \"$_debtap_tmp\"\n"
            << "  \"$_debtap_tmp\" \"$@\"\n"
            << "  _debtap_rc=$?\n"
            << "  rm -f \"$_debtap_tmp\"\n"
            << "  return \"$_debtap_rc\"\n"
            << "}\n\n";
    };
    for (const auto& item : scripts) emitRunner(item.first);

    // Keep each pacman hook unique; several Debian scripts map to the same upgrade hook.
    if (scripts.count("preinst"))
        out << "pre_install() { _debtap_run_preinst install; }\n";
    if (scripts.count("prerm"))
        out << "pre_remove() { _debtap_run_prerm remove; }\n";
    if (scripts.count("postinst"))
        out << "post_install() { _debtap_run_postinst configure; }\n";
    if (scripts.count("postrm"))
        out << "post_remove() { _debtap_run_postrm remove; }\n";
    if (scripts.count("preinst") || scripts.count("prerm")) {
        out << "\npre_upgrade() {\n";
        if (scripts.count("prerm")) out << "  _debtap_run_prerm upgrade || return $?\n";
        if (scripts.count("preinst")) out << "  _debtap_run_preinst upgrade || return $?\n";
        out << "}\n";
    }
    if (scripts.count("postinst") || scripts.count("postrm")) {
        out << "\npost_upgrade() {\n";
        if (scripts.count("postinst")) out << "  _debtap_run_postinst configure || return $?\n";
        if (scripts.count("postrm")) out << "  _debtap_run_postrm upgrade || return $?\n";
        out << "}\n";
    }
    return out.str();
}

std::uintmax_t contentSize(const fs::path& root) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec)) {
            auto n = it->file_size(ec);
            if (!ec) total += n;
            ec.clear();
        }
    }
    return total;
}

std::string readField(const Fields& fields, const std::string& name, const std::string& fallback = "") {
    auto it = fields.find(name);
    return it == fields.end() ? fallback : it->second;
}

std::vector<std::string> parseListIfPresent(const Fields& fields, const std::string& field,
                                             const std::map<std::string, std::string>& mapping,
                                             bool wipeout = false) {
    auto it = fields.find(field);
    if (it == fields.end()) return {};
    return uniqueSorted(parseDependencyList(it->second, mapping, wipeout));
}

void appendPkgInfoLines(std::ostringstream& out, const std::string& key, const std::vector<std::string>& values) {
    for (const auto& value : values) if (!value.empty()) out << key << " = " << value << "\n";
}

std::string cleanLicense(const Fields& fields) {
    (void)fields;
    // Debian's control file generally doesn't have license metadata; don't guess the license.
    return "unknown";
}

void writePkgInfo(const fs::path& root, const Fields& fields,
                  const std::string& pkgname, const std::string& pkgver,
                  const std::string& pkgrel, const std::string& arch,
                  const std::map<std::string, std::string>& mapping, bool wipeout) {
    std::ostringstream out;
    const std::string originalVersion = readField(fields, "version", "0");
    const std::string description = safeText(readField(fields, "description", pkgname));
    const std::string url = safeText(readField(fields, "homepage", ""));
    const std::string maintainer = safeText(readField(fields, "maintainer", "Unknown"));

    out << "# Generated by debtap-cpp\n";
    out << "pkgname = " << pkgname << "\n";
    out << "pkgbase = " << pkgname << "\n";
    out << "pkgver = " << pkgver << "-" << pkgrel << "\n";
    out << "pkgdesc = " << (description.empty() ? pkgname : description.substr(0, description.find('\n'))) << "\n";
    if (!url.empty()) out << "url = " << url << "\n";
    out << "builddate = " << static_cast<long long>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) << "\n";
    out << "packager = " << (maintainer.empty() ? "Unknown" : maintainer) << "\n";
    out << "size = " << contentSize(root) << "\n";
    out << "arch = " << arch << "\n";
    out << "license = " << cleanLicense(fields) << "\n";

    auto depends = parseListIfPresent(fields, "depends", mapping, wipeout);
    auto preDepends = parseListIfPresent(fields, "pre-depends", mapping, wipeout);
    depends.insert(depends.end(), preDepends.begin(), preDepends.end());
    depends = uniqueSorted(std::move(depends));
    appendPkgInfoLines(out, "depend", depends);

    auto recommends = parseListIfPresent(fields, "recommends", mapping, wipeout);
    auto suggests = parseListIfPresent(fields, "suggests", mapping, wipeout);
    for (const auto& dep : recommends) out << "optdepend = " << dep << ": recommended by Debian package metadata\n";
    for (const auto& dep : suggests) out << "optdepend = " << dep << ": suggested by Debian package metadata\n";
    appendPkgInfoLines(out, "conflict", parseListIfPresent(fields, "conflicts", mapping, wipeout));
    appendPkgInfoLines(out, "provides", parseListIfPresent(fields, "provides", mapping, wipeout));
    appendPkgInfoLines(out, "replaces", parseListIfPresent(fields, "replaces", mapping, wipeout));
    out << "xdata = debtap-cpp:debian-version=" << safeVersion(originalVersion) << "\n";
    writeFile(root / ".PKGINFO", out.str());
}

std::string makePkgbuild(const fs::path& deb, const Fields& fields,
                         const std::string& pkgname, const std::string& pkgver,
                         const std::string& pkgrel, const std::string& arch,
                         const std::map<std::string, std::string>& mapping,
                         bool hasInstallScript, bool wipeout) {
    const std::string desc = safeText(readField(fields, "description", pkgname));
    const std::string url = safeText(readField(fields, "homepage", ""));
    std::vector<std::string> depends = parseListIfPresent(fields, "depends", mapping, wipeout);
    auto preDepends = parseListIfPresent(fields, "pre-depends", mapping, wipeout);
    depends.insert(depends.end(), preDepends.begin(), preDepends.end());
    depends = uniqueSorted(std::move(depends));
    auto optDepends = parseListIfPresent(fields, "recommends", mapping, wipeout);
    auto suggests = parseListIfPresent(fields, "suggests", mapping, wipeout);
    optDepends.insert(optDepends.end(), suggests.begin(), suggests.end());
    optDepends = uniqueSorted(std::move(optDepends));
    auto conflicts = parseListIfPresent(fields, "conflicts", mapping, wipeout);
    auto provides = parseListIfPresent(fields, "provides", mapping, wipeout);
    auto replaces = parseListIfPresent(fields, "replaces", mapping, wipeout);

    std::ostringstream out;
    out << "# Generated by debtap-cpp. Review metadata and maintainer scripts before building/installing.\n";
    out << "pkgname=" << shellSingleQuote(pkgname) << "\n";
    out << "pkgver=" << shellSingleQuote(pkgver) << "\n";
    out << "pkgrel=" << shellSingleQuote(pkgrel) << "\n";
    out << "pkgdesc=" << shellSingleQuote(desc.empty() ? pkgname : desc.substr(0, desc.find('\n'))) << "\n";
    out << "arch=(" << shellSingleQuote(arch) << ")\n";
    if (!url.empty()) out << "url=" << shellSingleQuote(url) << "\n";
    out << "license=('unknown')\n";
    auto emitArray = [&](const std::string& key, const std::vector<std::string>& vals) {
        if (vals.empty()) return;
        out << key << "=(";
        for (const auto& v : vals) out << shellSingleQuote(v) << ' ';
        out << ")\n";
    };
    emitArray("depends", depends);
    emitArray("optdepends", optDepends);
    emitArray("conflicts", conflicts);
    emitArray("provides", provides);
    emitArray("replaces", replaces);
    if (hasInstallScript) out << "install=" << shellSingleQuote(pkgname + ".install") << "\n";
    out << "source=(" << shellSingleQuote("debtap-source.deb::" + urlEncodePath(deb)) << ")\n";
    out << "sha256sums=('SKIP')\n\n";
    out << "package() {\n";
    out << "  local _deb='debtap-source.deb'\n";
    out << "  local _work=\"$srcdir/deb-extract\"\n";
    out << "  rm -rf \"$_work\"\n";
    out << "  mkdir -p \"$_work/outer\" \"$pkgdir\"\n";
    out << "  bsdtar -xf \"$srcdir/$_deb\" --no-same-owner -C \"$_work/outer\"\n";
    out << "  local _data=(\"$_work/outer\"/data.tar.*)\n";
    out << "  [[ -f \"${_data[0]}\" ]] || { echo 'No data.tar.* member in .deb' >&2; return 1; }\n";
    out << "  bsdtar -xf \"${_data[0]}\" --no-same-owner -C \"$pkgdir\"\n";
    out << "  for _d in bin sbin usr/sbin usr/games usr/local/games usr/local/bin usr/local/sbin; do\n";
    out << "    if [[ -d \"$pkgdir/$_d\" ]]; then mkdir -p \"$pkgdir/usr/bin\"; find \"$pkgdir/$_d\" -mindepth 1 -maxdepth 1 -exec mv -t \"$pkgdir/usr/bin\" -- {} +; rm -rf \"$pkgdir/$_d\"; fi\n";
    out << "  done\n";
    out << "  for _d in lib lib64 usr/lib64; do\n";
    out << "    if [[ -d \"$pkgdir/$_d\" ]]; then mkdir -p \"$pkgdir/usr/lib\"; find \"$pkgdir/$_d\" -mindepth 1 -maxdepth 1 -exec mv -t \"$pkgdir/usr/lib\" -- {} +; rm -rf \"$pkgdir/$_d\"; fi\n";
    out << "  done\n";
    out << "  if [[ -d \"$pkgdir/usr/share/applications\" ]]; then find \"$pkgdir/usr/share/applications\" -type f -name '*.desktop' -print0 | while IFS= read -r -d '' _f; do sed -i -e 's#^Exec=/bin/#Exec=/usr/bin/#g' -e 's#^Exec=/sbin/#Exec=/usr/bin/#g' -e 's#^Exec=/usr/sbin/#Exec=/usr/bin/#g' -e 's#^Exec=/usr/games/#Exec=/usr/bin/#g' -e 's#^Exec=/usr/local/bin/#Exec=/usr/bin/#g' -e 's#^Exec=/usr/local/sbin/#Exec=/usr/bin/#g' -e 's#^Exec=/usr/local/games/#Exec=/usr/bin/#g' \"$_f\"; done; fi\n";
    out << "}\n";
    return out.str();
}

// Generating .MTREE file.
void createMtree(const fs::path& root) {
    std::vector<std::string> args = {
        "bsdtar", "-czf", (root / ".MTREE").string(), "--format=mtree",
        "--options=!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link",
        "-C", root.string()
    };
    // The input list is assembled before .MTREE exists, so the file doesn't include itself.
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.path().filename() == ".MTREE") continue;
        args.push_back(entry.path().filename().string());
    }
    std::sort(args.begin() + 7, args.end());
    auto result = runCommand(args);
    if (result.status != 0) {
        std::cerr << "[debtap-cpp] Warning: bsdtar could not create .MTREE; package will be produced without it.\n";
        std::error_code ec;
        fs::remove(root / ".MTREE", ec);
    }
}

// Creating the compressed Arch package archive (using zstd as pacman expects).
void createPackageArchive(const fs::path& root, const fs::path& output) {
    auto r = runCommand({"tar", "--zstd", "-cf", output.string(), "--numeric-owner", "--owner=0", "--group=0", "-C", root.string(), "."});
    if (r.status != 0) throw std::runtime_error("failed to create .pkg.tar.zst archive using tar --zstd");
}

void checkPackageWithPacman(const fs::path& package) {
    auto r = runCommand({"pacman", "-Qp", package.string()});
    if (r.status != 0) {
        std::cerr << "[debtap-cpp] Warning: pacman could not read the generated package. Inspect it manually.\n";
    }
}

Options parseArgs(int argc, char** argv) {
    Options o;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + name);
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: debtap-cpp [-qQswpP] [-o DIR] [OPTIONS] PACKAGE.deb\n"
                      << "Convert a Debian package into an Arch Linux .pkg.tar.zst package.\n\n"
                      << "Options:\n"
                      << "  -q, --quiet       Bypass interactive questions (this port does not prompt)\n"
                      << "  -Q, --Quiet       Bypass all interactive questions (this port does not prompt)\n"
                      << "  -s, --pseudo      Mark an i386 package as x86_64 (compatibility mode only)\n"
                      << "  -w, --wipeout     Drop versions from dependency/conflict metadata\n"
                      << "  -p, --pkgbuild    Also write a companion PKGBUILD\n"
                      << "  -P, --Pkgbuild    Write a PKGBUILD only; do not create a package archive\n"
                      << "  -o, --output DIR  Output directory (default: current directory)\n"
                      << "      --map FILE    Debian-to-Arch package-name mapping file\n"
                      << "      --keep-work   Keep temporary extraction files for inspection\n"
                      << "      --namcap      Run namcap on the generated package if installed\n"
                      << "  -f, --force       Overwrite output files\n"
                      << "  -v, --version     Print compatible debtap version marker (3.6.3)\n"
                      << "  -h, --help        Show this help\n\n"
                      << "Note: --update is not part of this conversion-only C++ port.\n";
            std::exit(0);
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "3.6.3-cpp\n";
            std::exit(0);
        } else if (arg == "-o" || arg == "--output") o.output = value(arg);
        else if (arg == "--map") o.mapFile = fs::path(value(arg));
        else if (arg == "--no-pkgbuild") o.writePkgbuild = false;
        else if (arg == "--pkgbuild") o.writePkgbuild = true;
        else if (arg == "--Pkgbuild") { o.writePkgbuild = true; o.pkgbuildOnly = true; }
        else if (arg == "--keep-work") o.keepWork = true;
        else if (arg == "--namcap") o.runNamcap = true;
        else if (arg == "--force") o.force = true;
        else if (arg == "--pseudo") o.pseudo = true;
        else if (arg == "--wipeout") o.wipeout = true;
        else if (arg == "--quiet") o.quiet = true;
        else if (arg == "--Quiet") { o.quiet = true; o.quietAll = true; }
        else if (!arg.empty() && arg[0] == '-' && arg.size() > 1 && arg[1] != '-') {
            // Match the original script's combined short-option style, e.g. -qwp.
            bool consumedOutput = false;
            for (std::size_t j = 1; j < arg.size(); ++j) {
                switch (arg[j]) {
                    case 'q': o.quiet = true; break;
                    case 'Q': o.quiet = true; o.quietAll = true; break;
                    case 's': o.pseudo = true; break;
                    case 'w': o.wipeout = true; break;
                    case 'p': o.writePkgbuild = true; break;
                    case 'P': o.writePkgbuild = true; o.pkgbuildOnly = true; break;
                    case 'f': o.force = true; break;
                    case 'o':
                        if (j + 1 < arg.size()) { o.output = arg.substr(j + 1); consumedOutput = true; j = arg.size(); }
                        else { o.output = value(arg); consumedOutput = true; }
                        break;
                    default: throw std::runtime_error("unknown short option: -" + std::string(1, arg[j]));
                }
            }
            (void)consumedOutput;
        } else if (arg.rfind("--", 0) == 0) throw std::runtime_error("unknown option: " + arg);
        else positional.push_back(arg);
    }
    if (positional.size() != 1) throw std::runtime_error("expected exactly one .deb input; use --help for usage");
    o.deb = fs::absolute(positional.front());
    if (!fs::is_regular_file(o.deb)) throw std::runtime_error("input is not a regular file: " + o.deb.string());
    if (o.deb.extension() != ".deb") std::cerr << "[debtap-cpp] Warning: input file extension is not .deb.\n";
    o.output = fs::absolute(o.output);
    return o;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options = parseArgs(argc, argv);
        requireCommand("bsdtar");
        requireCommand("tar");
        requireCommand("zstd");
        requireCommand("pacman");
        fs::create_directories(options.output);

        const auto nameMap = loadNameMap(options.mapFile);
        TempDirectory temp(options.keepWork);
        const fs::path outer = temp.path() / "outer";
        const fs::path controlDir = temp.path() / "control";
        const fs::path root = temp.path() / "pkgroot";
        fs::create_directories(outer);
        fs::create_directories(controlDir);
        fs::create_directories(root);

        // Defining package with full path & defining and creating working directory.
        // Extracting package data and metadata in working directory (also checking package architecture).
        std::cerr << "[debtap-cpp] Extracting Debian archive...\n";
        extractDebianContainer(options.deb, outer);
        auto controlMembers = findOuterArchiveMembers(outer, "control.tar");
        auto dataMembers = findOuterArchiveMembers(outer, "data.tar");
        if (controlMembers.empty()) throw std::runtime_error(".deb does not contain control.tar.*");
        if (dataMembers.empty()) throw std::runtime_error(".deb does not contain data.tar.*");
        extractMember(controlMembers.front(), controlDir);
        extractMember(dataMembers.front(), root);

        auto controlFile = findByFilename(controlDir, "control");
        if (!controlFile) throw std::runtime_error("could not find Debian control metadata in control.tar.*");
        Fields fields = parseControl(*controlFile);
        const std::string debName = readField(fields, "package");
        if (debName.empty()) throw std::runtime_error("Debian control file is missing the Package field");
        const std::string pkgname = translateName(debName, nameMap);
        const std::string originalVersion = readField(fields, "version", "0");
        const std::string pkgver = safeVersion(originalVersion);
        const std::string pkgrel = "1";
        const std::string debArchitecture = lower(readField(fields, "architecture", "all"));
        const bool isI386 = debArchitecture == "i386" || debArchitecture == "i486" ||
                            debArchitecture == "i586" || debArchitecture == "i686";
        if (options.pseudo && !isI386)
            throw std::runtime_error("--pseudo is intended only for i386 .deb packages");
        if (options.pseudo) {
#if !defined(__x86_64__)
            throw std::runtime_error("--pseudo requires an x86_64 host");
#endif
        }
        const std::string arch = options.pseudo ? "x86_64" : archName(debArchitecture);

        // Package-name translation corresponds to the original script's translator stage.
        if (nameMap.find(lower(debName)) == nameMap.end() && pkgname != lower(debName)) {
            std::cerr << "[debtap-cpp] Package name normalized from '" << debName << "' to '" << pkgname << "'.\n";
        }
        if (arch != "any") {
            const char* host = nullptr;
#if defined(__x86_64__)
            host = "x86_64";
#elif defined(__aarch64__)
            host = "aarch64";
#elif defined(__i386__)
            host = "i686";
#endif
            if (host && arch != host)
                std::cerr << "[debtap-cpp] Warning: package architecture " << arch << " differs from host " << host << ".\n";
        }

        // Fix Debian filesystem layout before generating package metadata.
        normalizeFilesystem(root, options.pseudo, debArchitecture);

        // Translate Debian maintainer scripts into pacman .INSTALL callbacks.
        std::map<std::string, std::string> scripts;
        for (const std::string script : {"preinst", "postinst", "prerm", "postrm"}) {
            auto p = findByFilename(controlDir, script);
            if (p) scripts[script] = readFile(*p);
        }
        if (!scripts.empty()) {
            std::string install = makeInstallScript(scripts);
            writeFile(root / ".INSTALL", install);
            std::cerr << "[debtap-cpp] Generated .INSTALL from Debian maintainer scripts. Review it before installation.\n";
        }

        // Build Arch package metadata, including dependencies and translated package names.
        if (!options.pkgbuildOnly) {
            // Generating .PKGINFO file.
            writePkgInfo(root, fields, pkgname, pkgver, pkgrel, arch, nameMap, options.wipeout);
            createMtree(root);
        }

        std::string filename = pkgname + "-" + pkgver + "-" + pkgrel + "-" + arch + ".pkg.tar.zst";
        fs::path packagePath = options.output / filename;
        fs::path pkgbuildPath = options.output / "PKGBUILD";
        fs::path installPath = options.output / (pkgname + ".install");
        if (!options.force) {
            if (!options.pkgbuildOnly && fs::exists(packagePath))
                throw std::runtime_error("output already exists (use --force): " + packagePath.string());
            if (options.writePkgbuild && fs::exists(pkgbuildPath))
                throw std::runtime_error("PKGBUILD already exists (use --force): " + pkgbuildPath.string());
            if (options.writePkgbuild && !scripts.empty() && fs::exists(installPath))
                throw std::runtime_error("install script already exists (use --force): " + installPath.string());
        }

        if (!options.pkgbuildOnly) {
            std::cerr << "[debtap-cpp] Creating Arch package archive...\n";
            createPackageArchive(root, packagePath);
            checkPackageWithPacman(packagePath);
        }

        if (options.writePkgbuild) {
            const bool hasInstall = !scripts.empty();
            const std::string recipe = makePkgbuild(options.deb, fields, pkgname, pkgver, pkgrel, arch, nameMap, hasInstall, options.wipeout);
            writeFile(pkgbuildPath, recipe);
            if (hasInstall) writeFile(installPath, makeInstallScript(scripts));
            std::cerr << "[debtap-cpp] Wrote PKGBUILD: " << pkgbuildPath << "\n";
            if (hasInstall) std::cerr << "[debtap-cpp] Wrote install script: " << installPath << "\n";
        }

        if (options.runNamcap && !options.pkgbuildOnly) {
            try {
                requireCommand("namcap");
                (void)runCommand({"namcap", packagePath.string()});
            } catch (const std::exception& e) {
                std::cerr << "[debtap-cpp] namcap unavailable or failed: " << e.what() << "\n";
            }
        }

        if (!options.pkgbuildOnly) {
            std::cout << "Created: " << packagePath << "\n";
            std::cout << "Package: " << pkgname << "\nVersion: " << pkgver << "-" << pkgrel << "\nArchitecture: " << arch << "\n";
            std::cout << "Next: inspect the package and generated maintainer script(s) before installing.\n";
        } else {
            std::cout << "Created PKGBUILD: " << pkgbuildPath << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "debtap-cpp: error: " << e.what() << "\n";
        return 1;
    }
}
