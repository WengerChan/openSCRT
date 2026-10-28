#include "transfer/modem/XyzModemEngine.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace openscrt::transfer::modem {
namespace {

namespace fs = std::filesystem;

constexpr std::uint8_t kCan = 0x18;

// The flags of a receiver, in the first byte of the answer to a request to send.
//
// CANFDX and CANOVIO say that the receiver can do both at once. TESCCTL says that
// the receiver expects the control characters to be escaped, which is true here: the
// engine unescapes every control character it is sent.
//
// CANFC32 is deliberately NOT advertised: the 16-bit checksum is what the protocol
// uses by default, and saying that the 32-bit one is fine would only make the far end
// use frames this engine would have to decode a second way.
constexpr std::uint8_t kReceiverFlags = 0x01 | 0x02 | 0x40;

// The high half of the window the receiver advertises: thirty-two means eight
// kilobytes.
constexpr std::uint8_t kReceiverWindowHigh = 32;

bool isFrameEnd(std::uint8_t byte) {
    return byte == static_cast<std::uint8_t>(FrameEnd::kEnd) ||
           byte == static_cast<std::uint8_t>(FrameEnd::kContinue) ||
           byte == static_cast<std::uint8_t>(FrameEnd::kQuery) ||
           byte == static_cast<std::uint8_t>(FrameEnd::kWait);
}

// The name and the details of a file, as they travel in the frame that follows the
// file header: the name, a zero, and then the size, the time and the mode.
bool parseFileInfo(const std::vector<std::uint8_t>& payload, ModemFileInfo& info) {
    const auto zero = std::find(payload.begin(), payload.end(), 0);
    if (zero == payload.end()) {
        return false;
    }
    info.name.assign(payload.begin(), zero);

    const auto second = std::find(zero + 1, payload.end(), 0);
    const std::string details(payload.begin() + (zero - payload.begin()) + 1,
                              second == payload.end() ? payload.end() : second);
    if (details.empty()) {
        return true;
    }
    // The size is in decimal, the time and the mode in octal: that is how the
    // reference implementation writes them and how it reads them.
    std::size_t at = 0;
    const auto number = [&details, &at](bool octal) -> unsigned long long {
        while (at < details.size() && std::isspace(static_cast<unsigned char>(details[at])) != 0) {
            ++at;
        }
        unsigned long long value = 0;
        while (at < details.size() && details[at] >= '0' && details[at] <= '9') {
            value =
                value * (octal ? 8ULL : 10ULL) + static_cast<unsigned long long>(details[at] - '0');
            ++at;
        }
        return value;
    };
    info.size = number(false);
    info.modified_time = static_cast<std::uint32_t>(number(true));
    info.mode = static_cast<std::uint16_t>(number(true));
    return true;
}

std::string fileInfoPayload(const ModemFileInfo& info) {
    std::string payload = info.name;
    payload.push_back('\0');
    payload += std::to_string(info.size);
    payload += ' ';
    payload += std::to_string(static_cast<unsigned long long>(info.modified_time));
    payload += ' ';
    payload += std::to_string(static_cast<unsigned long long>(info.mode));
    payload.push_back('\0');
    return payload;
}

}  // namespace

XyzModemEngine::XyzModemEngine(Options options) : options_(std::move(options)) {}

XyzModemEngine::~XyzModemEngine() {
    closeReceivedFile();
}

void XyzModemEngine::fail(const std::string& reason) {
    error_ = reason;
    phase_ = Phase::kFailed;
    state_ = ModemState::kFailed;
    closeReceivedFile();
}

void XyzModemEngine::closeReceivedFile() {
    if (output_.is_open()) {
        output_.close();
    }
}

std::vector<std::uint8_t> XyzModemEngine::cancel() {
    // Five cancels: that is how the protocol says "stop".
    std::vector<std::uint8_t> out(5, kCan);
    phase_ = Phase::kCancelled;
    state_ = ModemState::kCancelled;
    closeReceivedFile();
    return out;
}

bool XyzModemEngine::readHeader(ZmodemHeader& header, bool& crc32_frame, std::size_t& used) {
    // The decoding of a header lives with the frames, so that it can be tested on its
    // own: the bug it had was in the arithmetic of the digits, and a test that only
    // goes through a whole transfer would have taken ten minutes to find it.
    return decodeHeader(buffer_.data(), buffer_.size(), header, crc32_frame, used);
}

bool XyzModemEngine::readDataFrame(std::vector<std::uint8_t>& data, FrameEnd& frame_end,
                                   std::size_t& used) {
    data.clear();
    std::size_t at = 0;
    while (at < buffer_.size()) {
        const std::uint8_t byte = buffer_[at];
        if (byte == kZdle) {
            if (at + 1 >= buffer_.size()) {
                break;  // The rest of the escape has not arrived yet.
            }
            const std::uint8_t next = buffer_[at + 1];
            if (isFrameEnd(next)) {
                // The frame ends here. The checksum follows it.
                if (at + 3 >= buffer_.size()) {
                    break;
                }
                frame_end = static_cast<FrameEnd>(next);
                const auto sent =
                    static_cast<std::uint16_t>((buffer_[at + 2] << 8) | buffer_[at + 3]);
                if (dataChecksum(data.data(), data.size(), frame_end) != sent) {
                    return false;
                }
                used = at + 4;
                return true;
            }
            // An escaped byte: the top two bits are flipped.
            data.push_back(static_cast<std::uint8_t>(next ^ 0x40));
            at += 2;
            continue;
        }
        if (byte == 0x11 || byte == 0x13) {
            // The flow control characters are never part of the data.
            ++at;
            continue;
        }
        data.push_back(byte);
        ++at;
    }
    return false;
}

void XyzModemEngine::sendReceiverInit(std::vector<std::uint8_t>& out) {
    ZmodemHeader header;
    header.frame = ZmodemFrame::kReceiveInit;
    header.bytes[0] = kReceiverFlags;
    // The second byte is the high half of the window the far end may fill. The low half
    // is the flags, because the far end reads the window out of the same two bytes the
    // flags are in -- so the window is "the flags, plus eight kilobytes".
    //
    // Leaving the second byte at zero says "a window of sixty-seven bytes", and the far
    // end then sends the file in sixty-seven byte pieces: a hundred megabytes would be
    // a million and a half round trips.
    header.bytes[1] = kReceiverWindowHigh;
    const std::vector<std::uint8_t> bytes = encodeHexHeader(header);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void XyzModemEngine::sendSenderInit(std::vector<std::uint8_t>& out) {
    ZmodemHeader header;
    header.frame = ZmodemFrame::kRequestReceiveInit;
    const std::vector<std::uint8_t> bytes = encodeHexHeader(header);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void XyzModemEngine::sendResume(std::uint32_t position, std::vector<std::uint8_t>& out) {
    // The frame that says where to resume is a hex frame, even though it carries a
    // position. The reference implementation sends it that way and the other end
    // reads it that way.
    ZmodemHeader header;
    header.frame = ZmodemFrame::kResume;
    header.setPosition(position);
    const std::vector<std::uint8_t> bytes = encodeHexHeader(header);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void XyzModemEngine::sendAck(std::uint32_t position, std::vector<std::uint8_t>& out) {
    // The answer to a frame that ended with "the other end has to answer". The far end
    // waits for it, and until it comes the transfer does not move.
    ZmodemHeader header;
    header.frame = ZmodemFrame::kAck;
    header.setPosition(position);
    const std::vector<std::uint8_t> bytes = encodeHexHeader(header);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void XyzModemEngine::sendFileHeader(std::vector<std::uint8_t>& out) {
    ZmodemHeader header;
    header.frame = ZmodemFrame::kFile;
    header.bytes[0] = 0;  // The transfer is binary: no conversion of the line ends.
    const std::vector<std::uint8_t> head = encodeBinaryHeader(header, false);
    out.insert(out.end(), head.begin(), head.end());

    const std::string payload = fileInfoPayload(file_);
    const std::vector<std::uint8_t> info(payload.begin(), payload.end());
    const std::vector<std::uint8_t> body =
        encodeDataFrame(info.data(), info.size(), FrameEnd::kWait);
    out.insert(out.end(), body.begin(), body.end());
}

bool XyzModemEngine::openFileToSend() {
    if (options_.upload_path.empty()) {
        return false;
    }
    input_.open(options_.upload_path, std::ios::binary);
    if (!input_.good()) {
        return false;
    }
    input_.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(input_.tellg());
    input_.seekg(0);
    file_.name = std::filesystem::path(options_.upload_path).filename().string();
    file_.size = size;
    return true;
}

void XyzModemEngine::sendBlock(std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> block(options_.block_size);
    input_.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
    block.resize(static_cast<std::size_t>(input_.gcount()));

    ZmodemHeader header;
    header.frame = ZmodemFrame::kData;
    header.setPosition(static_cast<std::uint32_t>(transferred_));
    const std::vector<std::uint8_t> head = encodeBinaryHeader(header, false);
    out.insert(out.end(), head.begin(), head.end());

    // The frame ends with "more follows" while there is more, and with "the frame is
    // over" on the last one.
    const FrameEnd frame_end = input_.eof() ? FrameEnd::kEnd : FrameEnd::kContinue;
    const std::vector<std::uint8_t> body = encodeDataFrame(block.data(), block.size(), frame_end);
    out.insert(out.end(), body.begin(), body.end());
    transferred_ += block.size();
}

void XyzModemEngine::sendEndOfFile(std::vector<std::uint8_t>& out) {
    ZmodemHeader header;
    header.frame = ZmodemFrame::kEndOfFile;
    header.setPosition(static_cast<std::uint32_t>(transferred_));
    const std::vector<std::uint8_t> bytes = encodeBinaryHeader(header, false);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void XyzModemEngine::sendFinish(std::vector<std::uint8_t>& out) {
    ZmodemHeader header;
    header.frame = ZmodemFrame::kFinish;
    const std::vector<std::uint8_t> bytes = encodeHexHeader(header);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

bool XyzModemEngine::openReceivedFile() {
    closeReceivedFile();
    std::error_code error;
    const fs::path directory(options_.download_directory);
    if (!fs::is_directory(directory, error)) {
        fs::create_directories(directory, error);
    }
    // The name of the far end is not to be trusted: only the last part of it is used,
    // so a name with a path in it cannot write outside the directory.
    const std::string name = fs::path(file_.name).filename().string();
    output_path_ = (directory / (name.empty() ? std::string("zmodem.out") : name)).string();
    output_.open(output_path_, std::ios::binary | std::ios::trunc);
    return output_.good();
}

void XyzModemEngine::handleBlock(const std::vector<std::uint8_t>& data, FrameEnd frame_end,
                                 std::vector<std::uint8_t>& out) {
    if (phase_ == Phase::kAwaitFileInfo) {
        // The frame that follows the file header carries the name and the details.
        if (!parseFileInfo(data, file_)) {
            fail("the file header could not be read");
            return;
        }
        if (!openReceivedFile()) {
            fail("the file could not be written: " + output_path_);
            return;
        }
        transferred_ = 0;
        // The frame ends with "the other end has to answer", so it gets the answer
        // first and the position afterwards.
        sendAck(0, out);
        sendResume(0, out);
        phase_ = Phase::kReceiving;
        return;
    }
    if (phase_ == Phase::kReceiving) {
        if (!output_.is_open()) {
            fail("the file could not be written: " + output_path_);
            return;
        }
        output_.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        transferred_ += data.size();
        // A frame that ends with "the other end has to answer" gets the answer; the
        // position follows it, so the far end always has something to read.
        if (frame_end == FrameEnd::kWait || frame_end == FrameEnd::kQuery) {
            sendAck(static_cast<std::uint32_t>(transferred_), out);
        }
        sendResume(static_cast<std::uint32_t>(transferred_), out);
        return;
    }
}

void XyzModemEngine::handleHeader(const ZmodemHeader& header, std::vector<std::uint8_t>& out) {
    switch (header.frame) {
        case ZmodemFrame::kRequestReceiveInit:
            // The far end wants to send. We answer that we are ready.
            if (phase_ == Phase::kIdle) {
                sendReceiverInit(out);
                phase_ = Phase::kAwaitFileInfo;
                state_ = ModemState::kReceiving;
            }
            break;

        case ZmodemFrame::kReceiveInit:
            // The far end wants to receive. We are the sender now.
            if (phase_ == Phase::kIdle) {
                if (!openFileToSend()) {
                    fail("the file to send could not be opened: " + options_.upload_path);
                    return;
                }
                sendSenderInit(out);
                phase_ = Phase::kSending;
                state_ = ModemState::kSending;
            }
            break;

        case ZmodemFrame::kFile:
            // The far end is sending a file. The details follow in a data frame.
            if (phase_ == Phase::kAwaitFileInfo) {
                // The details are read by the caller.
            }
            break;

        case ZmodemFrame::kData:
            // The block follows in a data frame.
            break;

        case ZmodemFrame::kEndOfFile:
            if (phase_ == Phase::kReceiving) {
                // The file is over. The answer to the block was already sent with the
                // last block, and now the far end is told that we are ready for the
                // next file -- which is how the reference implementation ends a file.
                // Without that answer the far end waits for it until it gives up.
                closeReceivedFile();
                sendReceiverInit(out);
                phase_ = Phase::kAwaitFileInfo;
            }
            break;

        case ZmodemFrame::kResume:
            if (phase_ == Phase::kSending) {
                const std::uint32_t position = header.position();
                if (!file_header_sent_) {
                    // The first answer is to our request to send: the file header and
                    // the details of the file go out now.
                    sendFileHeader(out);
                    file_header_sent_ = true;
                    break;
                }
                // The other answers ask for the next block.
                if (position != 0) {
                    input_.clear();
                    input_.seekg(position);
                    transferred_ = position;
                }
                if (input_.eof()) {
                    sendEndOfFile(out);
                    break;
                }
                sendBlock(out);
            }
            break;

        case ZmodemFrame::kFinish:
            if (phase_ == Phase::kReceiving || phase_ == Phase::kAwaitFileInfo) {
                // The far end says goodbye. We say it back and wait for the two letters
                // that end the session.
                sendFinish(out);
                phase_ = Phase::kAwaitFinish;
            } else if (phase_ == Phase::kSending) {
                // The far end says goodbye to what we are sending: the two letters end
                // the session.
                out.push_back('O');
                out.push_back('O');
                phase_ = Phase::kDone;
                state_ = ModemState::kFinished;
            } else if (phase_ == Phase::kAwaitFinish) {
                // The far end did not see our goodbye. We say it again.
                sendFinish(out);
            }
            break;

        case ZmodemFrame::kCancel:
            phase_ = Phase::kCancelled;
            state_ = ModemState::kCancelled;
            closeReceivedFile();
            break;

        default:
            break;
    }
}

std::vector<std::uint8_t> XyzModemEngine::feed(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out;
    if (data == nullptr || size == 0) {
        return out;
    }
    if (phase_ == Phase::kDone || phase_ == Phase::kCancelled || phase_ == Phase::kFailed) {
        return out;
    }
    buffer_.insert(buffer_.end(), data, data + size);

    while (true) {
        if (phase_ == Phase::kAwaitFinish) {
            // The two letters that end the session. They can be anywhere in what is
            // left, so the buffer is searched for them.
            const std::string marker = "OO";
            const auto found =
                std::search(buffer_.begin(), buffer_.end(), marker.begin(), marker.end());
            if (found == buffer_.end()) {
                break;
            }
            buffer_.erase(
                buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(found - buffer_.begin()) + 2);
            phase_ = Phase::kDone;
            state_ = ModemState::kFinished;
            break;
        }

        ZmodemHeader header;
        bool crc32_frame = false;
        std::size_t used = 0;
        if (!readHeader(header, crc32_frame, used)) {
            break;
        }
        const std::size_t header_end = used;
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(header_end));

        // The frame that follows the header: the details of the file, or a block of it.
        if (header.frame == ZmodemFrame::kFile || header.frame == ZmodemFrame::kData) {
            std::vector<std::uint8_t> payload;
            FrameEnd frame_end = FrameEnd::kEnd;
            std::size_t data_used = 0;
            if (!readDataFrame(payload, frame_end, data_used)) {
                // The frame has not arrived yet. The header is put back so that the
                // whole thing is read again when the rest arrives.
                buffer_.insert(buffer_.begin(), buffer_.begin(),
                               buffer_.begin() + static_cast<std::ptrdiff_t>(header_end));
                break;
            }
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(data_used));
            if (header.frame == ZmodemFrame::kData && phase_ == Phase::kReceiving) {
                handleBlock(payload, frame_end, out);
            } else if (header.frame == ZmodemFrame::kFile) {
                phase_ = Phase::kAwaitFileInfo;
                handleBlock(payload, frame_end, out);
            }
            continue;
        }

        handleHeader(header, out);
    }

    return out;
}

}  // namespace openscrt::transfer::modem
