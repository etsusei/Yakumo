// Native macOS entry point for one user-operated paired-test session.
// The bundled Python supervisor owns run preparation, process lifecycle, and
// evidence packaging. This process only verifies and invokes that supervisor.

#include "psprecomp/sha256.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <mach-o/dyld.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr std::size_t kManifestBytes = 4096;
constexpr std::size_t kResultBytes = 256 * 1024;
constexpr std::size_t kDiagnosticBytes = 64 * 1024;
constexpr std::uintmax_t kPythonBytes = 256 * 1024 * 1024;
constexpr std::string_view kResultSchema = "yakumo-test-launch-result-v1";

struct Options {
    bool headless = false;
    bool prepare_only = false;
    std::optional<fs::path> python;
    std::optional<std::string> python_sha256;
    std::optional<fs::path> script;
    std::optional<fs::path> config;
};

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::string stdout_text;
    std::string stderr_text;
};

struct JsonScalar {
    enum class Kind { String, Boolean, Null, Other } kind = Kind::Other;
    std::string text;
};

std::string read_bounded(const fs::path &path, std::size_t limit) {
    if (!fs::is_regular_file(fs::symlink_status(path)))
        throw std::runtime_error("Missing or oversized regular file: " + path.string());
    const std::uintmax_t length = fs::file_size(path);
    if (length > limit)
        throw std::runtime_error("Missing or oversized regular file: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read file: " + path.string());
    std::string value(static_cast<std::size_t>(length), '\0');
    input.read(value.data(), static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length))
        throw std::runtime_error("File changed or exceeded its size limit: " + path.string());
    char extra = 0;
    if (input.get(extra) || !input.eof())
        throw std::runtime_error("File changed or exceeded its size limit: " + path.string());
    return value;
}

std::string one_line(std::string value, const char *label) {
    if (!value.empty() && value.back() == '\n') value.pop_back();
    if (value.empty() || value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw std::runtime_error(std::string("Invalid ") + label);
    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte == 127) throw std::runtime_error(std::string("Invalid ") + label);
    }
    return value;
}

bool valid_sha256(std::string_view digest) {
    if (digest.size() != 64) return false;
    for (char byte : digest)
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
    return true;
}

fs::path canonical_file(const fs::path &path, const char *label) {
    if (!path.is_absolute() || !fs::is_regular_file(path))
        throw std::runtime_error(std::string(label) + " must be an absolute regular file");
    const fs::path resolved = fs::canonical(path);
    if (resolved != path)
        throw std::runtime_error(std::string(label) + " contains a path alias or symlink");
    return resolved;
}

fs::path executable_path() {
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    if (length == 0 || length > 1024 * 1024)
        throw std::runtime_error("Cannot locate test launcher executable");
    std::vector<char> buffer(length);
    if (_NSGetExecutablePath(buffer.data(), &length) != 0)
        throw std::runtime_error("Cannot locate test launcher executable");
    return fs::canonical(buffer.data());
}

Options parse_options(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--headless") options.headless = true;
        else if (name == "--prepare-only") options.prepare_only = true;
        else {
            if (i + 1 >= argc) throw std::runtime_error("Missing value after " + std::string(name));
            const std::string value(argv[++i]);
            if (name == "--python" && !options.python) options.python = fs::path(value);
            else if (name == "--python-sha256" && !options.python_sha256) options.python_sha256 = value;
            else if (name == "--script" && !options.script) options.script = fs::path(value);
            else if (name == "--config" && !options.config) options.config = fs::path(value);
            else throw std::runtime_error("Unknown or repeated option: " + std::string(name));
        }
    }
    const bool overrides = options.python || options.python_sha256 || options.script || options.config;
    if (!options.headless && (options.prepare_only || overrides))
        throw std::runtime_error("Preparation and path overrides require --headless");
    return options;
}

void append_bounded(std::string &target, const char *data, std::size_t size,
                    std::size_t limit, bool &truncated) {
    const auto take = std::min(size, limit - target.size());
    target.append(data, take);
    if (take < size) truncated = true;
}

void drain_pipe(int &fd, std::string &target, std::size_t limit, bool &truncated) {
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            append_bounded(target, buffer, static_cast<std::size_t>(count), limit, truncated);
            continue;
        }
        if (count == 0) { close(fd); fd = -1; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            fd = -1;
            truncated = true;
        }
        return;
    }
}

void keep_pipe_above_stdio(int pipe_fds[2]) {
    for (int *slot : {&pipe_fds[0], &pipe_fds[1]}) {
        int &fd = *slot;
        if (fd >= 3) continue;
        const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (replacement < 0) throw std::runtime_error("Cannot reserve supervisor pipe descriptor");
        close(fd);
        fd = replacement;
    }
}

ProcessResult run_python(const fs::path &python, const std::vector<std::string> &arguments,
                         std::optional<std::chrono::seconds> timeout = std::nullopt,
                         std::size_t capture_limit = kDiagnosticBytes) {
    int out_pipe[2]{-1, -1};
    int err_pipe[2]{-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
        throw std::runtime_error("Cannot create supervisor diagnostic pipes");
    }
    try {
        keep_pipe_above_stdio(out_pipe);
        keep_pipe_above_stdio(err_pipe);
    } catch (...) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        throw;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

    std::vector<std::string> owned{"-E", "-s", "-B"};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv{const_cast<char *>(python.c_str())};
    for (std::string &item : owned) argv.push_back(item.data());
    argv.push_back(nullptr);
    // The supervisor forwards a small allowlist to the game. Apply that same
    // boundary before starting Python, so Python and its extension loader do
    // not inherit PYTHONPATH, PYTHONHOME, DYLD_* or game experiment switches.
    constexpr std::array<const char *, 16> allowed_environment{
        "HOME", "TMPDIR", "PATH", "LANG", "LC_ALL", "LC_CTYPE", "TERM",
        "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS", "PULSE_SERVER", "PIPEWIRE_REMOTE",
        "SDL_AUDIODRIVER", "SDL_VIDEODRIVER",
    };
    std::vector<std::string> environment;
    for (const char *name : allowed_environment) {
        const char *value = std::getenv(name);
        if (value && std::strlen(value) <= 8192)
            environment.emplace_back(std::string(name) + "=" + value);
    }
    std::vector<char *> envp;
    for (std::string &item : environment) envp.push_back(item.data());
    envp.push_back(nullptr);
    pid_t child = -1;
    const int spawn_error = posix_spawn(&child, python.c_str(), &actions, nullptr,
                                        argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawn_error != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        throw std::runtime_error("Cannot start pinned Python: " + std::string(std::strerror(spawn_error)));
    }
    for (int fd : {out_pipe[0], err_pipe[0]}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    ProcessResult result;
    const auto start = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> exited_at;
    int status = 0;
    bool reaped = false;
    for (;;) {
        if (!reaped) {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) { reaped = true; exited_at = std::chrono::steady_clock::now(); }
            else if (waited < 0 && errno != EINTR)
                throw std::runtime_error("Cannot wait for Python supervisor");
        }
        const auto now = std::chrono::steady_clock::now();
        if (timeout && !reaped && now - start > *timeout) {
            result.timed_out = true;
            kill(child, SIGTERM);
            for (int attempt = 0; attempt < 20 && !reaped; ++attempt) {
                usleep(100000);
                if (waitpid(child, &status, WNOHANG) == child) reaped = true;
            }
            if (!reaped) { kill(child, SIGKILL); waitpid(child, &status, 0); reaped = true; }
            exited_at = std::chrono::steady_clock::now();
        }
        if (reaped && out_pipe[0] < 0 && err_pipe[0] < 0) break;
        if (reaped && exited_at && now - *exited_at > std::chrono::seconds(2)) break;
        pollfd descriptors[2]{};
        nfds_t count = 0;
        if (out_pipe[0] >= 0) descriptors[count++] = {out_pipe[0], POLLIN | POLLHUP, 0};
        if (err_pipe[0] >= 0) descriptors[count++] = {err_pipe[0], POLLIN | POLLHUP, 0};
        const int ready = poll(descriptors, count, 100);
        if (ready < 0 && errno != EINTR) {
            if (!reaped) { kill(child, SIGKILL); waitpid(child, &status, 0); }
            if (out_pipe[0] >= 0) close(out_pipe[0]);
            if (err_pipe[0] >= 0) close(err_pipe[0]);
            throw std::runtime_error("Cannot read supervisor diagnostics");
        }
        if (out_pipe[0] >= 0) drain_pipe(out_pipe[0], result.stdout_text, capture_limit, result.stdout_truncated);
        if (err_pipe[0] >= 0) drain_pipe(err_pipe[0], result.stderr_text, capture_limit, result.stderr_truncated);
    }
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (!reaped) waitpid(child, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                       WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    return result;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    std::map<std::string, JsonScalar> top_level() {
        skip_space();
        expect('{');
        std::map<std::string, JsonScalar> fields;
        skip_space();
        if (consume('}')) return fields;
        for (;;) {
            skip_space();
            std::string key = parse_string();
            skip_space(); expect(':');
            JsonScalar value = parse_value(0);
            if (!fields.emplace(std::move(key), std::move(value)).second)
                throw std::runtime_error("Duplicate result JSON key");
            skip_space();
            if (consume('}')) break;
            expect(',');
        }
        skip_space();
        if (position_ != input_.size()) throw std::runtime_error("Trailing result JSON data");
        return fields;
    }

private:
    std::string_view input_;
    std::size_t position_ = 0;

    void skip_space() {
        while (position_ < input_.size() && (input_[position_] == ' ' ||
               input_[position_] == '\n' || input_[position_] == '\r' || input_[position_] == '\t')) ++position_;
    }
    bool consume(char value) {
        if (position_ < input_.size() && input_[position_] == value) { ++position_; return true; }
        return false;
    }
    void expect(char value) {
        skip_space();
        if (!consume(value)) throw std::runtime_error("Malformed result JSON");
    }
    static int hex(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        throw std::runtime_error("Malformed JSON Unicode escape");
    }
    unsigned unicode_word() {
        if (input_.size() - position_ < 4) throw std::runtime_error("Short JSON Unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) value = value * 16 + static_cast<unsigned>(hex(input_[position_++]));
        return value;
    }
    static void append_utf8(std::string &target, unsigned codepoint) {
        if (codepoint < 0x80) target.push_back(static_cast<char>(codepoint));
        else if (codepoint < 0x800) {
            target.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint < 0x10000) {
            target.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            target.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    std::string parse_string() {
        if (!consume('"')) throw std::runtime_error("Expected result JSON string");
        std::string value;
        while (position_ < input_.size()) {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') return value;
            if (byte < 32) throw std::runtime_error("Control byte in result JSON string");
            if (byte != '\\') { value.push_back(static_cast<char>(byte)); continue; }
            if (position_ == input_.size()) throw std::runtime_error("Short result JSON escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': case '\\': case '/': value.push_back(escaped); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = unicode_word();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (!consume('\\') || !consume('u'))
                        throw std::runtime_error("Missing JSON surrogate pair");
                    const unsigned low = unicode_word();
                    if (low < 0xdc00 || low > 0xdfff)
                        throw std::runtime_error("Invalid JSON surrogate pair");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
                    throw std::runtime_error("Unpaired JSON surrogate");
                append_utf8(value, codepoint);
                break;
            }
            default: throw std::runtime_error("Invalid result JSON escape");
            }
        }
        throw std::runtime_error("Unterminated result JSON string");
    }
    void keyword(std::string_view word) {
        if (input_.substr(position_, word.size()) != word)
            throw std::runtime_error("Malformed result JSON literal");
        position_ += word.size();
    }
    JsonScalar parse_value(unsigned depth) {
        if (depth > 32) throw std::runtime_error("Result JSON nesting limit exceeded");
        skip_space();
        if (position_ == input_.size()) throw std::runtime_error("Short result JSON");
        const char first = input_[position_];
        if (first == '"') return {JsonScalar::Kind::String, parse_string()};
        if (first == 't') { keyword("true"); return {JsonScalar::Kind::Boolean, "true"}; }
        if (first == 'f') { keyword("false"); return {JsonScalar::Kind::Boolean, "false"}; }
        if (first == 'n') { keyword("null"); return {JsonScalar::Kind::Null, {}}; }
        if (consume('[')) {
            skip_space();
            if (!consume(']')) for (;;) {
                parse_value(depth + 1);
                skip_space();
                if (consume(']')) break;
                expect(',');
            }
            return {};
        }
        if (consume('{')) {
            skip_space();
            if (!consume('}')) for (;;) {
                skip_space(); parse_string(); expect(':'); parse_value(depth + 1);
                skip_space();
                if (consume('}')) break;
                expect(',');
            }
            return {};
        }
        const auto start = position_;
        consume('-');
        if (consume('0')) {
            if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])))
                throw std::runtime_error("Invalid result JSON number");
        } else {
            if (position_ == input_.size() || input_[position_] < '1' || input_[position_] > '9')
                throw std::runtime_error("Invalid result JSON value");
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (consume('.')) {
            const auto fraction = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
            if (fraction == position_) throw std::runtime_error("Invalid result JSON fraction");
        }
        if (consume('e') || consume('E')) {
            if (!consume('+')) consume('-');
            const auto exponent = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
            if (exponent == position_) throw std::runtime_error("Invalid result JSON exponent");
        }
        if (position_ == start) throw std::runtime_error("Invalid result JSON number");
        return {};
    }
};

std::string field_string(const std::map<std::string, JsonScalar> &fields, std::string_view key) {
    const auto item = fields.find(std::string(key));
    return item != fields.end() && item->second.kind == JsonScalar::Kind::String ? item->second.text : "";
}

bool field_true(const std::map<std::string, JsonScalar> &fields, std::string_view key) {
    const auto item = fields.find(std::string(key));
    return item != fields.end() && item->second.kind == JsonScalar::Kind::Boolean && item->second.text == "true";
}

std::string translated(std::string_view english) {
    static const std::unordered_map<std::string_view, const char *> chinese{
#include "../host/ui/translations/zh_cn.inc"
    };
    const auto item = chinese.find(english);
    return item == chinese.end() ? std::string(english) : std::string(item->second);
}

CFStringRef cf_string(const std::string &utf8) {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8 *>(utf8.data()),
                                   static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
}

bool show_dialog(std::string_view title, std::string_view body, bool has_folder,
                 std::string_view detail = {}) {
    const std::string title_text = translated(title);
    std::string body_text = translated(body);
    if (!detail.empty()) {
        body_text += "\n\n" + translated("Details: ");
        for (char character : detail.substr(0, 500)) {
            const auto byte = static_cast<unsigned char>(character);
            body_text.push_back(byte < 32 || byte > 126 ? ' ' : character);
        }
    }
    const std::string close_text = translated("Close");
    const std::string open_text = translated("Open results folder");
    CFStringRef cf_title = cf_string(title_text);
    CFStringRef cf_body = cf_string(body_text);
    CFStringRef cf_close = cf_string(close_text);
    CFStringRef cf_open = has_folder ? cf_string(open_text) : nullptr;
    CFOptionFlags response = 0;
    const SInt32 error = CFUserNotificationDisplayAlert(
        0, kCFUserNotificationNoteAlertLevel, nullptr, nullptr, nullptr,
        cf_title, cf_body, cf_close, cf_open, nullptr, &response);
    if (cf_title) CFRelease(cf_title);
    if (cf_body) CFRelease(cf_body);
    if (cf_close) CFRelease(cf_close);
    if (cf_open) CFRelease(cf_open);
    if (error != 0) throw std::runtime_error("Cannot display test result dialog");
    return has_folder && (response & 0x3) == kCFUserNotificationAlternateResponse;
}

bool open_result_directory(const fs::path &python, const fs::path &script,
                           const fs::path &result_path) {
    const ProcessResult query = run_python(python, {script.string(), "--result-directory",
                                                     result_path.string()},
                                           std::chrono::seconds(10), 4096);
    if (query.exit_code != 0 || query.timed_out || query.stdout_truncated || query.stderr_truncated)
        return false;
    const std::string directory_text = one_line(query.stdout_text, "result directory reply");
    const fs::path directory(directory_text);
    if (!directory.is_absolute() || !fs::is_directory(directory) || fs::canonical(directory) != directory)
        return false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(directory_text.data()),
        static_cast<CFIndex>(directory_text.size()), true);
    if (!url) return false;
    const OSStatus error = LSOpenCFURLRef(url, nullptr);
    CFRelease(url);
    return error == noErr;
}

struct TempResult {
    fs::path directory;
    fs::path result_file;
    ~TempResult() {
        if (directory.empty()) return;
        std::error_code ignored;
        fs::remove(result_file, ignored);
        fs::remove(directory, ignored);
    }
};

TempResult new_private_result() {
    const char *temporary = std::getenv("TMPDIR");
    const fs::path base = fs::canonical(temporary && *temporary ? temporary : "/tmp");
    std::string pattern = (base / "yakumo-test-launch-XXXXXX").string();
    std::vector<char> chars(pattern.begin(), pattern.end());
    chars.push_back('\0');
    const char *created = mkdtemp(chars.data());
    if (!created) throw std::runtime_error("Cannot create private result directory");
    const fs::path directory(created);
    return {directory, directory / "result.json"};
}

std::string short_detail(const std::string &value) {
    std::string detail;
    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (detail.size() >= 500) break;
        detail.push_back(byte < 32 || byte == 127 ? ' ' : static_cast<char>(byte));
    }
    return detail;
}

int launch(const Options &options) {
    const fs::path executable = executable_path();
    const fs::path testing = executable.parent_path().parent_path() / "Resources/testing";
    fs::path python;
    std::string expected_hash;
    if (options.python) python = *options.python;
    else python = fs::path(one_line(read_bounded(testing / "python.path", kManifestBytes), "python.path"));
    if (options.python_sha256) expected_hash = *options.python_sha256;
    else expected_hash = one_line(read_bounded(testing / "python.sha256", 128), "python.sha256");
    if (!valid_sha256(expected_hash)) throw std::runtime_error("Invalid pinned Python SHA-256");
    python = canonical_file(python, "Python interpreter");
    if (fs::file_size(python) > kPythonBytes)
        throw std::runtime_error("Pinned Python interpreter exceeds size limit");
    if (psprecomp::sha256_file(python) != expected_hash)
        throw std::runtime_error("Pinned Python interpreter SHA-256 differs");
    if (access(python.c_str(), X_OK) != 0)
        throw std::runtime_error("Pinned Python interpreter is not executable");
    const fs::path script = canonical_file(options.script.value_or(testing / "launch_test_run.py"),
                                           "Supervisor script");
    const fs::path config = canonical_file(options.config.value_or(testing / "launch-config.json"),
                                           "Launch configuration");

    TempResult temporary = new_private_result();
    std::vector<std::string> command{script.string(), "--config", config.string(),
                                     "--result-file", temporary.result_file.string()};
    if (options.prepare_only) command.emplace_back("--prepare-only");
    const ProcessResult child = run_python(python, command);
    std::string status;
    std::string detail;
    bool valid_result = false;
    bool success = false;
    if (fs::exists(temporary.result_file)) {
        try {
            const auto fields = JsonReader(read_bounded(temporary.result_file, kResultBytes)).top_level();
            if (field_string(fields, "schema") != kResultSchema ||
                field_string(fields, "run_id").empty() ||
                field_string(fields, "run_directory").empty())
                throw std::runtime_error("Result schema or run identity is incomplete");
            status = field_string(fields, "status");
            if (status.empty()) throw std::runtime_error("Result status is missing");
            detail = field_string(fields, "error");
            valid_result = true;
            success = child.exit_code == 0 && !child.timed_out &&
                      (options.prepare_only ? status == "prepared" :
                       status == "completed" && field_true(fields, "evidence_complete") &&
                       field_true(fields, "recording_complete") &&
                       !field_string(fields, "package_path").empty());
        } catch (const std::exception &error) {
            detail = error.what();
        }
    } else detail = "Supervisor did not write a result file";
    if (!success && detail.empty()) {
        detail = "Result status: " + status;
        const std::string stop = child.stderr_text.empty() ? child.stdout_text : child.stderr_text;
        if (!stop.empty()) detail += "; supervisor: " + short_detail(stop);
    } else if (!success && !valid_result) {
        const std::string stop = child.stderr_text.empty() ? child.stdout_text : child.stderr_text;
        if (!stop.empty()) detail += "; supervisor: " + short_detail(stop);
    }
    std::cout << "Test launcher: " << (success ? "collected" : "incomplete")
              << ", supervisor exit " << child.exit_code;
    if (!status.empty()) std::cout << ", status " << status;
    std::cout << '\n';
    if (!success) std::cerr << "Test launcher detail: " << short_detail(detail) << '\n';
    if (child.stdout_truncated || child.stderr_truncated)
        std::cerr << "Supervisor diagnostic output was truncated at 64 KiB per stream.\n";
    if (options.headless) return success ? 0 : 1;

    const bool wants_folder = show_dialog(
        success ? "Test record collected" : "Test collection needs review",
        success ? "The record was collected and is awaiting comparison. Gameplay has not been checked."
                : "The test record is incomplete. Review the local results and diagnostics.",
        valid_result, success ? "" : detail);
    if (wants_folder) {
        if (!open_result_directory(python, script, temporary.result_file)) {
            show_dialog("Could not open results folder",
                        "The results folder could not be opened. Check the local diagnostics.", false);
            return 1;
        }
    }
    return success ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parse_options(argc, argv);
        return launch(options);
    } catch (const std::exception &error) {
        std::cerr << "Test launcher failed: " << error.what() << '\n';
        // Argument errors and incomplete local packaging are also visible when
        // Finder starts the application without a terminal.
        if (argc == 1) {
            try {
                show_dialog("Test launcher could not start",
                            "The test could not start. Check the local installation and diagnostics.",
                            false, error.what());
            } catch (...) { }
        }
        return 2;
    }
}
