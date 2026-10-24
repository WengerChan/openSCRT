#include "credential/keepasshttp/HttpTransport.h"

#include <QByteArray>
#include <QTcpSocket>

namespace openscrt::credential::keepasshttp {

QtHttpTransport::QtHttpTransport(QString host, quint16 port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

bool QtHttpTransport::post(const std::string& body, std::string& response, std::string& error) {
    QTcpSocket socket;
    socket.connectToHost(host_, port_);
    if (!socket.waitForConnected(timeout_ms_)) {
        error = "could not reach the KeePassHttp plugin on " + host_.toStdString() + ":" +
                std::to_string(port_);
        return false;
    }

    const QByteArray payload(body.data(), static_cast<qsizetype>(body.size()));
    QByteArray request;
    request += "POST / HTTP/1.1\r\n";
    request += "Host: " + host_.toLocal8Bit() + ":" + QByteArray::number(port_) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += payload;

    socket.write(request);
    socket.flush();
    if (!socket.waitForBytesWritten(timeout_ms_)) {
        error = "the request to the KeePassHttp plugin could not be sent";
        return false;
    }

    QByteArray raw;
    while (socket.waitForReadyRead(timeout_ms_)) {
        raw += socket.readAll();
    }
    raw += socket.readAll();
    if (raw.isEmpty()) {
        error = "the KeePassHttp plugin did not answer";
        return false;
    }

    const int separator = raw.indexOf("\r\n\r\n");
    if (separator < 0) {
        error = "the KeePassHttp plugin answered with something that is not HTTP";
        return false;
    }
    const QByteArray headers = raw.left(separator);
    const QByteArray body_bytes = raw.mid(separator + 4);

    if (!headers.startsWith("HTTP/1.1 200") && !headers.startsWith("HTTP/1.0 200")) {
        error = "the KeePassHttp plugin answered with an error: " +
                headers.left(headers.indexOf('\r')).toStdString();
        return false;
    }
    if (headers.contains("Transfer-Encoding: chunked", Qt::CaseInsensitive)) {
        error =
            "the KeePassHttp plugin answered with a chunked body, which this transport "
            "does not read";
        return false;
    }

    response.assign(body_bytes.constData(), static_cast<std::size_t>(body_bytes.size()));
    return true;
}

}  // namespace openscrt::credential::keepasshttp
