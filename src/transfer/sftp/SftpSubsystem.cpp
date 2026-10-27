#include "transfer/sftp/SftpSubsystem.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <cstdio>
#include <fstream>
#include <utility>

namespace openscrt::transfer::sftp {
namespace {

// The access flags of the SFTP protocol. They are the POSIX ones, and they are
// spelled out here because the local <fcntl.h> does not use the POSIX values on
// Windows -- and these flags are sent to the server, not applied to a local file.
constexpr int kReadOnly = 0;
constexpr int kWriteOnly = 1;
constexpr int kCreate = 0100;     // 64
constexpr int kTruncate = 01000;  // 512

// How much is read or written at a time. A megabyte is plenty for a line-speed
// connection and small enough not to sit in memory.
constexpr std::size_t kChunkSize = 64 * 1024;

std::string errorText(sftp_session sftp) {
    if (sftp == nullptr) {
        return "the sftp subsystem is not open";
    }
    // sftp_get_error() answers with a number, not with words. The words are on the
    // ssh session underneath it.
    const char* reason = ssh_get_error(sftp->session);
    if (reason != nullptr && *reason != '\0') {
        return reason;
    }
    return "the sftp server said no";
}

std::optional<SftpFileInfo> toFileInfo(const sftp_attributes attributes) {
    if (attributes == nullptr) {
        return std::nullopt;
    }
    SftpFileInfo info;
    if (attributes->name != nullptr) {
        info.name = attributes->name;
    }
    if (attributes->longname != nullptr) {
        info.long_name = attributes->longname;
    }
    info.size = attributes->size;
    // The mode of the file, with the file-type bits still in it.
    info.permissions = attributes->permissions;
    // The type is the code of the SFTP protocol, not a POSIX mode: libssh has already
    // translated the mode into it.
    info.is_directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
    info.is_regular_file = attributes->type == SSH_FILEXFER_TYPE_REGULAR;
    return info;
}

}  // namespace

SftpSubsystem::~SftpSubsystem() {
    close();
}

bool SftpSubsystem::open(protocols::ssh::SshNativeSession session, std::string& error) {
    if (session_ != nullptr) {
        error = "the sftp subsystem is already open";
        return false;
    }
    if (!session.valid()) {
        error = "the ssh session is not connected";
        return false;
    }

    auto* native = static_cast<ssh_session>(session.get());
    sftp_session sftp = sftp_new(native);
    if (sftp == nullptr) {
        const char* reason = ssh_get_error(native);
        error = std::string("the sftp subsystem could not be opened: ") +
                (reason != nullptr && *reason != '\0' ? reason : "the server refused it");
        return false;
    }
    // The version exchange. It is what tells the two ends which version of the
    // protocol they speak.
    if (sftp_init(sftp) != SSH_OK) {
        const std::string reason = errorText(sftp);
        sftp_free(sftp);
        error = "the sftp subsystem did not answer: " + reason;
        return false;
    }
    session_ = sftp;
    return true;
}

void SftpSubsystem::close() {
    if (session_ == nullptr) {
        return;
    }
    sftp_free(static_cast<sftp_session>(session_));
    session_ = nullptr;
}

std::string SftpSubsystem::homeDirectory(std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return {};
    }
    auto* sftp = static_cast<sftp_session>(session_);
    char* path = sftp_canonicalize_path(sftp, ".");
    if (path == nullptr) {
        error = errorText(sftp);
        return {};
    }
    std::string home(path);
    ssh_string_free_char(path);
    return home;
}

std::vector<SftpFileInfo> SftpSubsystem::listDirectory(const std::string& path,
                                                       std::string& error) {
    std::vector<SftpFileInfo> entries;
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return entries;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    sftp_dir directory = sftp_opendir(sftp, path.c_str());
    if (directory == nullptr) {
        error = errorText(sftp);
        return entries;
    }
    while (true) {
        sftp_attributes attributes = sftp_readdir(sftp, directory);
        if (attributes == nullptr) {
            break;
        }
        std::optional<SftpFileInfo> info = toFileInfo(attributes);
        sftp_attributes_free(attributes);
        if (!info) {
            continue;
        }
        // The two entries that point at the directory itself are noise in a file
        // manager.
        if (info->name == "." || info->name == "..") {
            continue;
        }
        entries.push_back(*info);
    }
    sftp_closedir(directory);
    return entries;
}

std::optional<SftpFileInfo> SftpSubsystem::stat(const std::string& path, std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return std::nullopt;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    sftp_attributes attributes = sftp_stat(sftp, path.c_str());
    if (attributes == nullptr) {
        error = errorText(sftp);
        return std::nullopt;
    }
    std::optional<SftpFileInfo> info = toFileInfo(attributes);
    sftp_attributes_free(attributes);
    return info;
}

std::string SftpSubsystem::canonicalPath(const std::string& path, std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return {};
    }
    auto* sftp = static_cast<sftp_session>(session_);
    char* canonical = sftp_canonicalize_path(sftp, path.c_str());
    if (canonical == nullptr) {
        error = errorText(sftp);
        return {};
    }
    std::string result(canonical);
    ssh_string_free_char(canonical);
    return result;
}

bool SftpSubsystem::download(const std::string& remote_path, const std::string& local_path,
                             std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return false;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    sftp_file file = sftp_open(sftp, remote_path.c_str(), kReadOnly, 0);
    if (file == nullptr) {
        error = errorText(sftp);
        return false;
    }
    std::ofstream local(local_path, std::ios::binary | std::ios::trunc);
    if (!local) {
        sftp_close(file);
        error = "the local file could not be written: " + local_path;
        return false;
    }
    std::vector<char> buffer(kChunkSize);
    while (true) {
        const ssize_t count = sftp_read(file, buffer.data(), buffer.size());
        if (count < 0) {
            sftp_close(file);
            error = errorText(sftp);
            return false;
        }
        if (count == 0) {
            break;
        }
        local.write(buffer.data(), count);
        if (!local) {
            sftp_close(file);
            error = "the local file could not be written: " + local_path;
            return false;
        }
    }
    sftp_close(file);
    return true;
}

bool SftpSubsystem::upload(const std::string& local_path, const std::string& remote_path,
                           std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return false;
    }
    std::ifstream local(local_path, std::ios::binary);
    if (!local) {
        error = "the local file could not be read: " + local_path;
        return false;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    sftp_file file = sftp_open(sftp, remote_path.c_str(), kWriteOnly | kCreate | kTruncate, 0644);
    if (file == nullptr) {
        error = errorText(sftp);
        return false;
    }
    std::vector<char> buffer(kChunkSize);
    while (true) {
        local.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = local.gcount();
        if (count <= 0) {
            break;
        }
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(count)) {
            const ssize_t accepted = sftp_write(file, buffer.data() + written,
                                                static_cast<std::size_t>(count) - written);
            if (accepted <= 0) {
                sftp_close(file);
                error = errorText(sftp);
                return false;
            }
            written += static_cast<std::size_t>(accepted);
        }
    }
    sftp_close(file);
    return true;
}

bool SftpSubsystem::remove(const std::string& path, std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return false;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    if (sftp_unlink(sftp, path.c_str()) != SSH_OK) {
        error = errorText(sftp);
        return false;
    }
    return true;
}

bool SftpSubsystem::makeDirectory(const std::string& path, std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return false;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    if (sftp_mkdir(sftp, path.c_str(), 0755) != SSH_OK) {
        error = errorText(sftp);
        return false;
    }
    return true;
}

bool SftpSubsystem::removeDirectory(const std::string& path, std::string& error) {
    if (session_ == nullptr) {
        error = "the sftp subsystem is not open";
        return false;
    }
    auto* sftp = static_cast<sftp_session>(session_);
    if (sftp_rmdir(sftp, path.c_str()) != SSH_OK) {
        error = errorText(sftp);
        return false;
    }
    return true;
}

}  // namespace openscrt::transfer::sftp
