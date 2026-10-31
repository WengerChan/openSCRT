// The X/Y/Zmodem engine (P3-T7).
//
// The engine is the state machine of a Zmodem transfer. It is the side of the
// conversation that the terminal emulator steps aside for: while a transfer is going
// on, the bytes belong to the engine and not to the screen.
//
// The protocol was read off the reference implementation -- lrzsz -- rather than off
// a description of it, because the details that matter are the ones nobody writes
// down. Three of them:
//
//   * the request to send and the answer to it are hex frames, the file and its data
//     are binary frames, and the goodbye is a hex frame again;
//   * the "where do we resume" frame is a hex frame too, even though it carries a
//     position;
//   * the file is over when the far end sends the end-of-file frame, and the answer
//     to it was already sent with the last block.
//
// The two directions are the same machinery. When the far end runs "sz file", we are
// the receiver: we answer the request to send, we ask for the file by name, we take
// the data frame by frame, and we finish with the two letters that end a Zmodem
// session. When the far end runs "rz", we are the sender and the steps are the
// other way round.

#ifndef OPENCRT_TRANSFER_MODEM_XYZ_MODEM_ENGINE_H_
#define OPENCRT_TRANSFER_MODEM_XYZ_MODEM_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "transfer/modem/ZmodemFrame.h"

namespace openscrt::transfer::modem {

enum class ModemState {
    kIdle,       // nothing is happening yet
    kReceiving,  // the far end is sending a file
    kSending,    // we are sending a file
    kFinished,   // the session is over
    kCancelled,  // the other end gave up
    kFailed,
};

struct ModemFileInfo {
    std::string name;
    std::uint64_t size{0};
    std::uint32_t modified_time{0};
    std::uint16_t mode{0};
};

class XyzModemEngine {
 public:
    struct Options {
        // Where a received file is written.
        std::string download_directory{"."};
        // The file we send. Empty means we only ever receive.
        std::string upload_path;
        // How much of the file travels in one frame.
        std::size_t block_size{1024};
    };

    explicit XyzModemEngine(Options options);
    ~XyzModemEngine();

    XyzModemEngine(const XyzModemEngine&) = delete;
    XyzModemEngine& operator=(const XyzModemEngine&) = delete;

    // Feeds the bytes that arrived and returns the bytes that have to go back to the
    // far end.
    std::vector<std::uint8_t> feed(const std::uint8_t* data, std::size_t size);

    // Gives up on the transfer. The other end sees five cancels and stops.
    [[nodiscard]] std::vector<std::uint8_t> cancel();

    [[nodiscard]] ModemState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const ModemFileInfo& currentFile() const noexcept { return file_; }
    [[nodiscard]] std::uint64_t bytesTransferred() const noexcept { return transferred_; }
    // The file the engine wrote, so the caller can say where it went.
    [[nodiscard]] const std::string& receivedPath() const noexcept { return output_path_; }
    // A probe: where the first frame whose checksum did not match was found, counted
    // in the bytes that arrived. It goes away with the bug it was put in for.
    [[nodiscard]] bool sawBadFrame() const noexcept { return saw_bad_frame_; }
    [[nodiscard]] std::size_t badFrameOffset() const noexcept { return bad_frame_offset_; }
    // How many frames arrived with a checksum that does not match. The far end sends
    // a few of them (it puts sixty stale bytes in front of a block from time to
    // time), and they are stepped over.
    [[nodiscard]] std::size_t badFrames() const noexcept { return bad_frames_; }

 private:
    enum class Phase {
        kIdle,
        kAwaitFileInfo,  // the far end sent the file header; the details follow
        kReceiving,      // the blocks of the file are coming
        kAwaitFinish,    // we said goodbye and wait for the two letters
        kSending,        // we are the sender
        kDone,
        kCancelled,
        kFailed,
    };

    // Reads one header out of the buffer. Returns false when there is not enough yet.
    [[nodiscard]] bool readHeader(ZmodemHeader& header, bool& crc32_frame, std::size_t& used);
    // What a read of a data frame found.
    enum class FrameRead {
        kComplete,    // A frame with a matching checksum.
        kIncomplete,  // Not enough bytes yet; wait for more.
        kBad,         // The frame is there, but its checksum does not match.
    };
    // Reads the data frame that follows a header.
    [[nodiscard]] FrameRead readDataFrame(std::vector<std::uint8_t>& data, FrameEnd& frame_end,
                                          std::size_t& used);

    void handleHeader(const ZmodemHeader& header, std::vector<std::uint8_t>& out);
    void handleBlock(const std::vector<std::uint8_t>& data, FrameEnd frame_end,
                     std::vector<std::uint8_t>& out);

    void fail(const std::string& reason);
    void sendReceiverInit(std::vector<std::uint8_t>& out);
    void sendSenderInit(std::vector<std::uint8_t>& out);
    void sendResume(std::uint32_t position, std::vector<std::uint8_t>& out);
    void sendAck(std::uint32_t position, std::vector<std::uint8_t>& out);
    void sendFileHeader(std::vector<std::uint8_t>& out);
    void sendBlock(std::vector<std::uint8_t>& out);
    void sendEndOfFile(std::vector<std::uint8_t>& out);
    void sendFinish(std::vector<std::uint8_t>& out);

    [[nodiscard]] bool openReceivedFile();
    void closeReceivedFile();
    [[nodiscard]] bool openFileToSend();

    Options options_;
    Phase phase_{Phase::kIdle};
    ModemState state_{ModemState::kIdle};
    std::string error_;
    // The probe of the bad frame: how many bytes left the buffer, where the first
    // frame whose checksum did not match was found, and how many there were.
    std::size_t consumed_{0};
    bool saw_bad_frame_{false};
    std::size_t bad_frame_offset_{0};
    std::size_t bad_frames_{0};
    // True while the sender is sending a run of data frames. The data header
    // introduces the run, and the frames of the run follow one another with no
    // header between them.
    bool in_data_run_{false};
    ModemFileInfo file_;
    std::uint64_t transferred_{0};

    std::vector<std::uint8_t> buffer_;
    std::ofstream output_;
    std::string output_path_;
    std::ifstream input_;
    bool file_header_sent_{false};
};

}  // namespace openscrt::transfer::modem

#endif  // OPENCRT_TRANSFER_MODEM_XYZ_MODEM_ENGINE_H_
