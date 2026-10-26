// The interactive command mode of the SFTP tab (P3-T5).
//
// The commands are the ones a SecureCRT SFTP window knows: lpwd, lcd, ls, cd, get,
// put, mget, mput. A command is a line of text, and the answer is a line of text --
// which is what makes the whole thing testable without a screen in front of it.
//
// The remote half is the subsystem; the local half is the filesystem of this machine.

#ifndef OPENCRT_TRANSFER_SFTP_SFTP_SHELL_H_
#define OPENCRT_TRANSFER_SFTP_SFTP_SHELL_H_

#include <string>
#include <vector>

#include "transfer/sftp/SftpSubsystem.h"

namespace openscrt::transfer::sftp {

class SftpShell {
 public:
    explicit SftpShell(SftpSubsystem& subsystem);

    // Runs one command line and returns what the pane shows. An unknown command is
    // answered, not ignored: a person who types a command and sees nothing has no
    // way of telling a wrong command from a broken program.
    std::vector<std::string> execute(const std::string& line);

    [[nodiscard]] const std::string& localDirectory() const noexcept { return local_directory_; }
    [[nodiscard]] const std::string& remoteDirectory() const noexcept { return remote_directory_; }

    void setLocalDirectory(std::string path);
    void setRemoteDirectory(std::string path);

    // True after "quit". The pane closes itself then.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

 private:
    std::vector<std::string> runLocal(const std::vector<std::string>& words);
    std::vector<std::string> runRemote(const std::vector<std::string>& words);
    std::vector<std::string> transfer(const std::vector<std::string>& words, bool upload);
    [[nodiscard]] std::vector<std::string> listLocal();
    [[nodiscard]] std::vector<std::string> listRemote(const std::string& path);

    SftpSubsystem& subsystem_;
    std::string local_directory_;
    std::string remote_directory_;
    bool finished_{false};
};

// Splits a command line into words. Quoted words keep their spaces.
std::vector<std::string> splitCommandLine(const std::string& line);

// The names in a directory that match a pattern. The pattern may hold one "*", which
// stands for any number of characters.
std::vector<std::string> matchingNames(const std::vector<std::string>& names,
                                       const std::string& pattern);

}  // namespace openscrt::transfer::sftp

#endif  // OPENCRT_TRANSFER_SFTP_SFTP_SHELL_H_
