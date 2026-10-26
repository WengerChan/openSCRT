#include "transfer/sftp/SftpShell.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace openscrt::transfer::sftp {
namespace {

namespace fs = std::filesystem;

// The help, written once so the command and the answer cannot drift apart.
const char* const kHelp =
    "lpwd            the local directory\n"
    "lcd <path>      change the local directory\n"
    "ls [path]       list the far side\n"
    "cd <path>       change the far side\n"
    "pwd             the directory on the far side\n"
    "get <file> [local]   copy a file down\n"
    "put <file> [remote]  copy a file up\n"
    "mget <pattern>  copy several files down\n"
    "mput <pattern>  copy several files up\n"
    "help            this list\n"
    "quit            leave";

bool matchesPattern(const std::string& name, const std::string& pattern) {
    const std::size_t star = pattern.find('*');
    if (star == std::string::npos) {
        return name == pattern;
    }
    const std::string prefix = pattern.substr(0, star);
    const std::string suffix = pattern.substr(star + 1);
    if (name.size() < prefix.size() + suffix.size()) {
        return false;
    }
    return name.compare(0, prefix.size(), prefix) == 0 &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::vector<std::string> splitCommandLine(const std::string& line) {
    std::vector<std::string> words;
    std::string current;
    bool in_quotes = false;
    bool has_word = false;

    for (const char character : line) {
        if (character == '"') {
            in_quotes = !in_quotes;
            has_word = true;
            continue;
        }
        if (!in_quotes && (character == ' ' || character == '\t')) {
            if (has_word) {
                words.push_back(current);
                current.clear();
                has_word = false;
            }
            continue;
        }
        current.push_back(character);
        has_word = true;
    }
    if (has_word) {
        words.push_back(current);
    }
    return words;
}

std::vector<std::string> matchingNames(const std::vector<std::string>& names,
                                       const std::string& pattern) {
    std::vector<std::string> matches;
    for (const std::string& name : names) {
        if (matchesPattern(name, pattern)) {
            matches.push_back(name);
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

SftpShell::SftpShell(SftpSubsystem& subsystem) : subsystem_(subsystem) {
    std::error_code error;
    local_directory_ = fs::current_path(error).string();
    if (error) {
        local_directory_ = ".";
    }
    std::string reason;
    remote_directory_ = subsystem_.homeDirectory(reason);
    if (remote_directory_.empty()) {
        remote_directory_ = ".";
    }
}

void SftpShell::setLocalDirectory(std::string path) {
    local_directory_ = std::move(path);
}

void SftpShell::setRemoteDirectory(std::string path) {
    remote_directory_ = std::move(path);
}

std::vector<std::string> SftpShell::execute(const std::string& line) {
    const std::vector<std::string> words = splitCommandLine(line);
    if (words.empty()) {
        return {};
    }

    const std::string& command = words[0];
    if (command == "quit" || command == "exit") {
        finished_ = true;
        return {"bye"};
    }
    if (command == "help" || command == "?") {
        return {kHelp};
    }
    if (command == "lpwd") {
        return {local_directory_};
    }
    if (command == "lcd") {
        return runLocal(words);
    }
    if (command == "pwd") {
        return {remote_directory_};
    }
    if (command == "cd") {
        return runRemote(words);
    }
    if (command == "ls") {
        return listRemote(words.size() > 1 ? words[1] : remote_directory_);
    }
    if (command == "get" || command == "mget" || command == "put" || command == "mput") {
        return transfer(words, command == "put" || command == "mput");
    }
    return {"unknown command: " + command + " -- type help for the list"};
}

std::vector<std::string> SftpShell::runLocal(const std::vector<std::string>& words) {
    if (words.size() < 2) {
        return {"lcd needs a path"};
    }
    std::error_code error;
    const fs::path wanted = fs::path(words[1]).is_absolute()
                                ? fs::path(words[1])
                                : fs::path(local_directory_) / words[1];
    if (!fs::is_directory(wanted, error)) {
        return {"there is no directory: " + wanted.string()};
    }
    local_directory_ = fs::canonical(wanted, error).string();
    if (error) {
        local_directory_ = wanted.string();
    }
    return {local_directory_};
}

std::vector<std::string> SftpShell::runRemote(const std::vector<std::string>& words) {
    if (words.size() < 2) {
        return {"cd needs a path"};
    }
    std::string error;
    const std::string canonical = subsystem_.canonicalPath(words[1], error);
    if (canonical.empty()) {
        return {error};
    }
    remote_directory_ = canonical;
    return {remote_directory_};
}

std::vector<std::string> SftpShell::listLocal() {
    std::vector<std::string> lines;
    std::error_code error;
    const fs::path directory(local_directory_);
    if (!fs::is_directory(directory, error)) {
        return {"there is no directory: " + local_directory_};
    }
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        std::error_code size_error;
        const auto size = entry.is_regular_file(size_error) ? entry.file_size(size_error) : 0;
        lines.push_back(entry.path().filename().string() + "  " + std::to_string(size));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::vector<std::string> SftpShell::listRemote(const std::string& path) {
    std::vector<std::string> lines;
    std::string error;
    const std::vector<SftpFileInfo> entries = subsystem_.listDirectory(path, error);
    if (!error.empty()) {
        return {error};
    }
    for (const SftpFileInfo& entry : entries) {
        lines.push_back(entry.name + "  " + std::to_string(entry.size));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::vector<std::string> SftpShell::transfer(const std::vector<std::string>& words, bool upload) {
    if (words.size() < 2) {
        return {upload ? "put needs a file" : "get needs a file"};
    }
    std::vector<std::string> results;

    if (upload) {
        // One name or a pattern.
        std::vector<std::string> names;
        if (words[1].find('*') != std::string::npos) {
            std::error_code error;
            const fs::path directory(local_directory_);
            for (const auto& entry : fs::directory_iterator(directory, error)) {
                if (error) {
                    break;
                }
                names.push_back(entry.path().filename().string());
            }
            names = matchingNames(names, words[1]);
        } else {
            names.push_back(words[1]);
        }
        for (const std::string& name : names) {
            const std::string local = (fs::path(local_directory_) / name).string();
            const std::string remote =
                (remote_directory_.empty() || remote_directory_.back() == '/')
                    ? remote_directory_ + name
                    : remote_directory_ + "/" + name;
            std::string error;
            if (subsystem_.upload(local, remote, error)) {
                results.push_back("sent " + name);
            } else {
                results.push_back(name + ": " + error);
            }
        }
        return results;
    }

    // Getting: one name or a pattern on the far side.
    std::vector<std::string> names;
    if (words[1].find('*') != std::string::npos) {
        std::string error;
        const std::vector<SftpFileInfo> entries =
            subsystem_.listDirectory(remote_directory_, error);
        if (!error.empty()) {
            return {error};
        }
        std::vector<std::string> all;
        for (const SftpFileInfo& entry : entries) {
            if (entry.is_regular_file) {
                all.push_back(entry.name);
            }
        }
        names = matchingNames(all, words[1]);
    } else {
        names.push_back(words[1]);
    }
    for (const std::string& name : names) {
        const std::string remote = (remote_directory_.empty() || remote_directory_.back() == '/')
                                       ? remote_directory_ + name
                                       : remote_directory_ + "/" + name;
        const std::string local = (fs::path(local_directory_) / name).string();
        std::string error;
        if (subsystem_.download(remote, local, error)) {
            results.push_back("got " + name);
        } else {
            results.push_back(name + ": " + error);
        }
    }
    return results;
}

}  // namespace openscrt::transfer::sftp
