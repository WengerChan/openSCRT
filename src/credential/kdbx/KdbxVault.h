// The open database (P2-T7).
//
// The vault is what sits between a .kdbx file and the sessions that need credentials
// from it. It holds the key material and the index, it watches the file, and it locks
// itself.
//
// The watching matters: a password database is edited outside the terminal, and a
// session that asks for a credential should get the one in the file now, not the one
// that was in the file when the window opened. The file changing is normal, not an
// error, so the vault re-reads it and says so.
//
// The locking matters more. The key material lives in memory only while the vault is
// open; after an idle period, or when the application is minimised to the tray, it is
// dropped and the vault holds nothing but the file name.
//
// The password is kept in locked memory while the vault is open, because re-reading a
// changed file needs it again. It is wiped together with the master key when the vault
// locks.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX_VAULT_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX_VAULT_H_

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/SecureByteArray.h"
#include "credential/kdbx/KdbxXmlIndex.h"

namespace openscrt::credential::kdbx {

class KdbxVault final : public QObject {
    Q_OBJECT

 public:
    explicit KdbxVault(QObject* parent = nullptr);
    ~KdbxVault() override;

    KdbxVault(const KdbxVault&) = delete;
    KdbxVault& operator=(const KdbxVault&) = delete;

    // Opens the database and starts watching the file. The key file is optional.
    [[nodiscard]] bool open(const QString& path, const std::uint8_t* password,
                            std::size_t password_size, const std::vector<std::uint8_t>& key_file,
                            std::string& error);

    // Drops the key material and the index. The file stays watched, so unlocking again
    // is just another open().
    void lock() noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] const KdbxIndex& index() const noexcept { return index_; }
    [[nodiscard]] QString path() const noexcept { return path_; }

    // The idle timeout: the vault locks itself after this long without being used.
    // Zero means it never locks itself. The unit is milliseconds so that a test can
    // wait for it; the window works in minutes.
    void setIdleTimeout(std::chrono::milliseconds timeout);
    [[nodiscard]] std::chrono::milliseconds idleTimeout() const noexcept { return idle_timeout_; }

    // Called when the vault is used, so the idle timer starts over.
    void touch();

    // The lookups the session layer needs.
    [[nodiscard]] const KdbxEntry* findByUuid(const std::string& uuid) const noexcept;
    [[nodiscard]] const KdbxEntry* findByHost(const std::string& host) const noexcept;

    // Turns an entry's password into plaintext, in the caller's locked buffer. The
    // caller owns it and wipes it when the connection attempt is over.
    [[nodiscard]] bool revealPassword(const KdbxEntry& entry, core::SecureByteArray& out,
                                      std::string& error) const;

 signals:
    // The database on disk changed and was re-read -- or the re-read failed, in which
    // case the vault still holds the previous index.
    void reloaded(bool ok, const QString& message);
    // The vault locked itself: the key material is gone.
    void locked();

 private:
    bool readDatabase(std::string& error);
    void onFileChanged(const QString& changed);
    void onIdleTimeout();
    void restartIdleTimer();
    // Drops the key material without telling anyone. The destructor uses it: a signal
    // emitted from a destructor reaches slots that may already be gone.
    void clear() noexcept;

    QString path_;
    core::SecureByteArray password_;
    std::vector<std::uint8_t> key_file_;
    KdbxIndex index_;
    KdbxProtectedStream protected_stream_;
    bool open_{false};
    std::chrono::milliseconds idle_timeout_{0};
    QFileSystemWatcher watcher_;
    QTimer idle_timer_;
};

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX_VAULT_H_
