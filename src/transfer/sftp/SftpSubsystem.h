// The SFTP subsystem (P3-T4).
//
// The subsystem is opened on the SSH session that is already connected: the same
// session, the same authentication, the same keys. The user does not type a password
// a second time, because there is no second authentication -- the SFTP subsystem is
// another channel on a connection that is already open.
//
// The calls block. The SFTP protocol is request and answer, so a call waits for its
// answer, and the caller is expected to be a worker thread and not the window.

#ifndef OPENCRT_TRANSFER_SFTP_SFTP_SUBSYSTEM_H_
#define OPENCRT_TRANSFER_SFTP_SFTP_SUBSYSTEM_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protocols/ssh/SshNativeSession.h"

namespace openscrt::transfer::sftp {

struct SftpFileInfo {
    std::string name;
    // The line "ls -l" would print. It is what a file manager shows, and parsing it
    // ourselves would be a worse version of what the server already knows.
    std::string long_name;
    std::uint64_t size{0};
    std::uint32_t permissions{0};
    bool is_directory{false};
    bool is_regular_file{false};
};

class SftpSubsystem {
 public:
    SftpSubsystem() = default;
    ~SftpSubsystem();

    SftpSubsystem(const SftpSubsystem&) = delete;
    SftpSubsystem& operator=(const SftpSubsystem&) = delete;

    // Opens the subsystem on a session that is already connected. The handle keeps the
    // session alive; the subsystem has to be closed before the SSH connection goes
    // away.
    bool open(ssh::SshNativeSession session, std::string& error);
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return session_ != nullptr; }

    // The directory the far side considers the home of the session.
    [[nodiscard]] std::string homeDirectory(std::string& error);

    // The entries of a directory, without the two entries "." and "..".
    [[nodiscard]] std::vector<SftpFileInfo> listDirectory(const std::string& path,
                                                          std::string& error);

    [[nodiscard]] std::optional<SftpFileInfo> stat(const std::string& path, std::string& error);

    // The path the far side uses for this one: the home directory becomes "~", a
    // relative path becomes an absolute one.
    [[nodiscard]] std::string canonicalPath(const std::string& path, std::string& error);

    bool download(const std::string& remote_path, const std::string& local_path,
                  std::string& error);
    bool upload(const std::string& local_path, const std::string& remote_path, std::string& error);

    bool remove(const std::string& path, std::string& error);
    bool makeDirectory(const std::string& path, std::string& error);
    bool removeDirectory(const std::string& path, std::string& error);

 private:
    // The libssh subsystem. The type stays behind this header.
    void* session_{nullptr};
};

}  // namespace openscrt::transfer::sftp

#endif  // OPENCRT_TRANSFER_SFTP_SFTP_SUBSYSTEM_H_
