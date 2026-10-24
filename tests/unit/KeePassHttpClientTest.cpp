// KeePassHTTP client tests (P2-T8).
//
// The tests talk to a mock server that speaks the real protocol over a real socket:
// it decrypts what the client sends and encrypts what it answers, so the test is an
// end-to-end check of the protocol rather than of a fake transport.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "credential/keepasshttp/HttpTransport.h"
#include "credential/keepasshttp/KeePassHttpClient.h"
#include "credential/keepasshttp/KeePassHttpCrypto.h"

namespace openscrt::credential::keepasshttp {
namespace {

using nlohmann::json;

// A mock of the KeePassHttp plugin: it answers the three requests of the protocol,
// with the entries encrypted the way the plugin encrypts them.
//
// It runs on its own thread. The client is synchronous -- it blocks in
// waitForReadyRead -- and that only pumps the events of *its* socket: without a thread
// of its own the mock would never see the request, and the test would fail for a
// reason that has nothing to do with the protocol.
class MockKeePassHttpServer final {
 public:
    MockKeePassHttpServer() {
        worker_ = std::make_unique<QThread>();
        QObject::connect(worker_.get(), &QThread::started, [this]() { start(); });
        worker_->start();
        listening_.acquire();  // wait until the server is listening
    }

    ~MockKeePassHttpServer() {
        worker_->quit();
        worker_->wait();
    }

    [[nodiscard]] quint16 port() const { return port_; }

    // The url the client asked for, decrypted by the mock.
    [[nodiscard]] std::string askedUrl() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return asked_url;
    }

    // The login the plugin answers with.
    std::string login = "netops";
    std::string password = "s3cret";

 private:
    void start() {
        server_ = new QTcpServer();
        server_->listen(QHostAddress::LocalHost, 0);
        port_ = server_->serverPort();
        QObject::connect(server_, &QTcpServer::newConnection, [this]() { onNewConnection(); });
        listening_.release();
    }

    void onNewConnection() {
        QTcpSocket* socket = server_->nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() { handle(socket); });
    }

    void handle(QTcpSocket* socket) {
        buffer_ += socket->readAll();
        int separator = buffer_.indexOf("\r\n\r\n");
        if (separator < 0) {
            return;
        }
        const QByteArray headers = buffer_.left(separator);
        const int length_header = headers.indexOf("Content-Length:");
        if (length_header < 0) {
            return;
        }
        const int length = headers.mid(length_header + 15).split('\r').first().trimmed().toInt();
        if (buffer_.size() < separator + 4 + length) {
            return;
        }
        const QByteArray body = buffer_.mid(separator + 4, length);
        buffer_.remove(0, separator + 4 + length);

        json response = answer(json::parse(body.toStdString()));
        const QByteArray payload = QByteArray::fromStdString(response.dump());
        QByteArray reply = "HTTP/1.1 200 OK\r\n";
        reply += "Content-Type: application/json\r\n";
        reply += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
        reply += "Connection: close\r\n\r\n";
        reply += payload;
        socket->write(reply);
        socket->flush();
        socket->disconnectFromHost();
    }

    json answer(const json& request) {
        const std::string type = request.value("RequestType", "");
        if (type == "associate") {
            key_ = request.value("Key", "");
            return {{"Id", "mock-key-id"}};
        }
        if (type == "test-associate") {
            return {{"Result", true}};
        }
        if (type == "get-logins") {
            const std::string nonce = request.value("Nonce", "");
            std::string error;
            std::string url;
            (void)decryptField(key_, nonce, request.value("Url", ""), url, error);
            asked_url = url;

            // The answer is encrypted with a nonce of the plugin's own.
            std::uint8_t raw[16];
            randombytes_buf(raw, sizeof(raw));
            const std::string response_nonce = base64(raw, sizeof(raw));

            std::string encrypted_password;
            std::string encrypted_login;
            std::string encrypted_name;
            (void)encryptField(key_, response_nonce, password, encrypted_password, error);
            (void)encryptField(key_, response_nonce, login, encrypted_login, error);
            (void)encryptField(key_, response_nonce, "router", encrypted_name, error);

            std::string encrypted_field;
            (void)encryptField(key_, response_nonce, "the value", encrypted_field, error);

            json entries = json::array();
            entries.push_back({{"Name", encrypted_name},
                               {"Login", encrypted_login},
                               {"Password", encrypted_password},
                               {"StringFields", json{{"Note", encrypted_field}}}});
            return {{"Nonce", response_nonce}, {"Entries", entries}};
        }
        return {{"Result", false}};
    }

    std::string key_;
    QByteArray buffer_;
    std::string asked_url;
    // The lock is mutable so that the getter can be const: reading the state does not
    // change it, but locking does.
    mutable std::mutex mutex_;
    std::unique_ptr<QThread> worker_;
    QTcpServer* server_{nullptr};
    quint16 port_{0};
    QSemaphore listening_;
};

class KeePassHttpFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        if (QCoreApplication::instance() == nullptr) {
            static int argc = 1;
            static char program[] = "keepasshttp-test";
            static char* argv[] = {program, nullptr};
            application_ = std::make_unique<QCoreApplication>(argc, argv);
        }
        server_ = std::make_unique<MockKeePassHttpServer>();
        transport_ =
            std::make_shared<QtHttpTransport>(QStringLiteral("127.0.0.1"), server_->port(), 5000);
        client_ = std::make_unique<KeePassHttpClient>(transport_);
    }

    std::unique_ptr<QCoreApplication> application_;
    std::unique_ptr<MockKeePassHttpServer> server_;
    std::shared_ptr<QtHttpTransport> transport_;
    std::unique_ptr<KeePassHttpClient> client_;
};

TEST_F(KeePassHttpFixture, TheAssociationGetsAnId) {
    Association association;
    std::string error;
    ASSERT_TRUE(client_->associate(association, error)) << error;
    EXPECT_EQ(association.key_id, "mock-key-id");
    EXPECT_FALSE(association.key_base64.empty());
}

TEST_F(KeePassHttpFixture, TheAssociationCanBeTested) {
    Association association;
    std::string error;
    ASSERT_TRUE(client_->associate(association, error)) << error;
    EXPECT_TRUE(client_->testAssociate(association, error)) << error;
}

TEST_F(KeePassHttpFixture, TheEntriesComeBackDecrypted) {
    Association association;
    std::string error;
    ASSERT_TRUE(client_->associate(association, error)) << error;

    std::vector<KeePassHttpEntry> entries;
    ASSERT_TRUE(
        client_->getLogins(association, "ssh://10.20.30.40:22", "10.20.30.40", entries, error))
        << error;

    // The mock decrypted what the client sent, so the request carried a real url.
    EXPECT_EQ(server_->askedUrl(), "ssh://10.20.30.40:22");

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().name, "router");
    EXPECT_EQ(entries.front().login, "netops");
    EXPECT_EQ(entries.front().password, "s3cret");
    ASSERT_EQ(entries.front().string_fields.size(), 1u);
    EXPECT_EQ(entries.front().string_fields.front().second, "the value");
}

TEST_F(KeePassHttpFixture, APluginThatIsNotThereIsReported) {
    // A port nobody listens on: the client has to say so rather than hang or crash.
    QtHttpTransport dead(QStringLiteral("127.0.0.1"), 1, 500);
    KeePassHttpClient client(std::make_shared<QtHttpTransport>(dead));
    Association association;
    std::string error;
    EXPECT_FALSE(client.associate(association, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(KeePassHttpFixture, AWrongKeyIsReported) {
    Association association;
    std::string error;
    ASSERT_TRUE(client_->associate(association, error)) << error;

    // An association with a key the plugin never saw: the plugin answers with an
    // error, and the client has to pass that on.
    Association wrong = association;
    wrong.key_base64 =
        base64(reinterpret_cast<const std::uint8_t*>("0123456789abcdef0123456789abcdef"), 32);
    std::vector<KeePassHttpEntry> entries;
    const bool ok =
        client_->getLogins(wrong, "ssh://10.20.30.40:22", "10.20.30.40", entries, error);
    // The mock decrypts with the key it was given, so the answer does not decrypt on
    // this side: the client must report a failure rather than return nonsense.
    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace openscrt::credential::keepasshttp
