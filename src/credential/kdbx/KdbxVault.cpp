#include "credential/kdbx/KdbxVault.h"

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <algorithm>
#include <utility>

#include "credential/kdbx/Kdbx3Reader.h"
#include "credential/kdbx/Kdbx4Reader.h"
#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

std::vector<std::uint8_t> readWholeFile(const QString& path) {
    std::vector<std::uint8_t> bytes;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return bytes;
    }
    const qint64 size = file.size();
    if (size <= 0) {
        return bytes;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const qint64 read = file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (read != size) {
        bytes.clear();
    }
    return bytes;
}

}  // namespace

KdbxVault::KdbxVault(QObject* parent) : QObject(parent) {
    idle_timer_.setSingleShot(true);
    connect(&idle_timer_, &QTimer::timeout, this, &KdbxVault::onIdleTimeout);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &KdbxVault::onFileChanged);
}

KdbxVault::~KdbxVault() {
    // The worker reads the members of the vault, so it has to be finished before they
    // are gone.
    if (open_worker_.joinable()) {
        open_worker_.join();
    }
    // No signal from a destructor: the slots are not necessarily still there.
    clear();
}

bool KdbxVault::open(const QString& path, const std::uint8_t* password, std::size_t password_size,
                     const std::vector<std::uint8_t>& key_file, std::string& error) {
    path_ = path;
    password_ = core::SecureByteArray();
    if (password != nullptr && password_size > 0) {
        password_.assign(password, password_size);
    }
    key_file_ = key_file;

    if (!readDatabase(error)) {
        path_.clear();
        password_ = core::SecureByteArray();
        key_file_.clear();
        return false;
    }

    open_ = true;
    if (!watcher_.files().contains(path_)) {
        watcher_.addPath(path_);
    }
    restartIdleTimer();
    return true;
}

void KdbxVault::openAsync(const QString& path, const std::uint8_t* password,
                          std::size_t password_size, const std::vector<std::uint8_t>& key_file) {
    if (open_worker_.joinable()) {
        // An unlock that is still running has to be finished before another one starts.
        open_worker_.join();
    }

    core::SecureByteArray secret;
    if (password != nullptr && password_size > 0) {
        secret.assign(password, password_size);
    }
    const std::vector<std::uint8_t> keys = key_file;

    open_worker_ = std::thread([this, path, secret = std::move(secret), keys]() mutable {
        PendingOpen result;
        std::string error;
        result.ok =
            readDatabaseInto(path, secret, keys, result.index, result.stream, &derivations_, error);
        result.error = std::move(error);
        result.path = path;
        // The password is kept: the file may change, and the vault has to read it again
        // without asking the user for the password a second time.
        result.password = std::move(secret);
        result.key_file = keys;
        {
            const std::lock_guard<std::mutex> lock(open_mutex_);
            pending_open_ = std::move(result);
        }
        // The result is picked up on the thread that owns the vault. The call carries
        // nothing but the vault itself, so it does not depend on what a queued
        // invocation is allowed to hold.
        QMetaObject::invokeMethod(this, [this]() { finishOpen(); }, Qt::QueuedConnection);
    });
}

void KdbxVault::finishOpen() {
    PendingOpen result;
    {
        const std::lock_guard<std::mutex> lock(open_mutex_);
        if (!pending_open_.has_value()) {
            return;
        }
        result = std::move(*pending_open_);
        pending_open_.reset();
    }

    if (!result.ok) {
        emit opened(false, QString::fromStdString(result.error));
        return;
    }

    path_ = result.path;
    password_ = std::move(result.password);
    key_file_ = std::move(result.key_file);
    index_ = std::move(result.index);
    protected_stream_ = std::move(result.stream);
    open_ = true;

    const QFileInfo info(path_);
    read_at_ = info.lastModified();
    read_size_ = info.size();

    if (!watcher_.files().contains(path_)) {
        watcher_.addPath(path_);
    }
    restartIdleTimer();
    emit opened(true, QString());
}

void KdbxVault::lock() noexcept {
    const bool was_open = open_;
    clear();
    if (was_open) {
        emit locked();
    }
}

void KdbxVault::clear() noexcept {
    open_ = false;
    index_ = KdbxIndex();
    protected_stream_ = KdbxProtectedStream();
    key_file_.clear();
    // The password is the last thing to go: the vault may be reopened with it.
    password_ = core::SecureByteArray();
    idle_timer_.stop();
}

void KdbxVault::setIdleTimeout(std::chrono::milliseconds timeout) {
    idle_timeout_ = timeout;
    restartIdleTimer();
}

void KdbxVault::touch() {
    restartIdleTimer();
    // Using the vault is a good moment to notice a change the watcher missed.
    checkFileChanged();
}

void KdbxVault::checkFileChanged() {
    if (!open_ || path_.isEmpty()) {
        return;
    }
    const QFileInfo info(path_);
    if (!info.exists() || (info.lastModified() == read_at_ && info.size() == read_size_)) {
        return;
    }
    std::string error;
    if (readDatabase(error)) {
        restartIdleTimer();
        emit reloaded(true, QString());
    } else {
        emit reloaded(false, QString::fromStdString(error));
    }
}

void KdbxVault::restartIdleTimer() {
    idle_timer_.stop();
    if (!open_ || idle_timeout_.count() <= 0) {
        return;
    }
    idle_timer_.start(static_cast<int>(idle_timeout_.count()));
}

bool KdbxVault::readDatabaseInto(const QString& path, const core::SecureByteArray& password,
                                 const std::vector<std::uint8_t>& key_file, KdbxIndex& index,
                                 KdbxProtectedStream& stream, std::atomic<std::size_t>* derivations,
                                 std::string& error) {
    const std::vector<std::uint8_t> bytes = readWholeFile(path);
    if (bytes.empty()) {
        error = "the database file could not be read";
        return false;
    }

    const KdbxHeaderParseResult header = parseKdbxHeader(bytes.data(), bytes.size());
    if (!header.ok()) {
        error = header.error;
        return false;
    }

    KdbxIndex new_index;
    KdbxProtectedStream new_stream;
    std::size_t xml_offset = 0;

    if (header.header.version == KdbxVersion::kV3_1) {
        if (derivations != nullptr) {
            derivations->fetch_add(1, std::memory_order_relaxed);
        }
        const Kdbx3ReadResult read =
            readKdbx3(bytes.data(), bytes.size(), password.data(), password.size(), key_file);
        if (!read.ok()) {
            error = read.error;
            return false;
        }
        new_stream = KdbxProtectedStream(read.protected_stream_id, read.protected_stream_key.data(),
                                         read.protected_stream_key.size());
        xml_offset = read.xml_offset;
        const KdbxIndexResult indexed =
            indexKdbxXml(read.payload.data() + xml_offset, read.payload.size() - xml_offset);
        if (!indexed.ok()) {
            error = indexed.error;
            return false;
        }
        new_index = indexed.index;
    } else {
        if (derivations != nullptr) {
            derivations->fetch_add(1, std::memory_order_relaxed);
        }
        const Kdbx4ReadResult read =
            readKdbx4(bytes.data(), bytes.size(), password.data(), password.size(), key_file);
        if (!read.ok()) {
            error = read.error;
            return false;
        }
        new_stream = KdbxProtectedStream(read.protected_stream_id, read.protected_stream_key.data(),
                                         read.protected_stream_key.size());
        xml_offset = read.xml_offset;
        const KdbxIndexResult indexed =
            indexKdbxXml(read.payload.data() + xml_offset, read.payload.size() - xml_offset);
        if (!indexed.ok()) {
            error = indexed.error;
            return false;
        }
        new_index = indexed.index;
    }

    index = std::move(new_index);
    stream = std::move(new_stream);
    return true;
}

bool KdbxVault::readDatabase(std::string& error) {
    KdbxIndex new_index;
    KdbxProtectedStream new_stream;
    if (!readDatabaseInto(path_, password_, key_file_, new_index, new_stream, &derivations_,
                          error)) {
        return false;
    }
    index_ = std::move(new_index);
    protected_stream_ = std::move(new_stream);
    const QFileInfo info(path_);
    read_at_ = info.lastModified();
    read_size_ = info.size();
    return true;
}

void KdbxVault::onFileChanged(const QString& changed) {
    if (!open_ || changed != path_) {
        return;
    }
    // Some editors save by writing a new file over the old one, which makes the watcher
    // drop the path. Put it back, or the next change goes unnoticed.
    if (!watcher_.files().contains(path_)) {
        watcher_.addPath(path_);
    }

    std::string error;
    if (readDatabase(error)) {
        touch();
        emit reloaded(true, QString());
        return;
    }
    emit reloaded(false, QString::fromStdString(error));
}

void KdbxVault::onIdleTimeout() {
    lock();
}

const KdbxEntry* KdbxVault::findByUuid(const std::string& uuid) const noexcept {
    for (const KdbxEntry& entry : index_.entries) {
        if (entry.uuid == uuid) {
            return &entry;
        }
    }
    return nullptr;
}

const KdbxEntry* KdbxVault::findByHost(const std::string& host) const noexcept {
    for (const KdbxEntry& entry : index_.entries) {
        if (entry.url == host || entry.url == "ssh://" + host) {
            return &entry;
        }
    }
    return nullptr;
}

bool KdbxVault::revealPassword(const KdbxEntry& entry, core::SecureByteArray& out,
                               std::string& error) const {
    return revealKdbxPassword(entry, protected_stream_, out, error);
}

}  // namespace openscrt::credential::kdbx
