#include "credential/kdbx/KdbxVault.h"

#include <QFile>
#include <algorithm>

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
    lock();
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

void KdbxVault::lock() noexcept {
    const bool was_open = open_;
    open_ = false;
    index_ = KdbxIndex();
    protected_stream_ = KdbxProtectedStream();
    key_file_.clear();
    // The password is the last thing to go: the vault may be reopened with it.
    password_ = core::SecureByteArray();
    idle_timer_.stop();
    if (was_open) {
        emit locked();
    }
}

void KdbxVault::setIdleTimeout(std::chrono::milliseconds timeout) {
    idle_timeout_ = timeout;
    restartIdleTimer();
}

void KdbxVault::touch() {
    restartIdleTimer();
}

void KdbxVault::restartIdleTimer() {
    idle_timer_.stop();
    if (!open_ || idle_timeout_.count() <= 0) {
        return;
    }
    idle_timer_.start(static_cast<int>(idle_timeout_.count()));
}

bool KdbxVault::readDatabase(std::string& error) {
    const std::vector<std::uint8_t> bytes = readWholeFile(path_);
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
        const Kdbx3ReadResult read =
            readKdbx3(bytes.data(), bytes.size(), password_.data(), password_.size(), key_file_);
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
        const Kdbx4ReadResult read =
            readKdbx4(bytes.data(), bytes.size(), password_.data(), password_.size(), key_file_);
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

    index_ = std::move(new_index);
    protected_stream_ = new_stream;
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
