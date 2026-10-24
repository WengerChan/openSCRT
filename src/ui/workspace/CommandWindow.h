// The command window (P2-T10).
//
// The bar at the bottom of the window where a command is typed. It has two modes: the
// command goes to the session in front, or -- with the broadcast on -- to every session
// at once.
//
// The broadcast reaches the sessions in the order they were opened, so twenty machines
// get the same command in the same order. That is the point of the feature: a command
// that means "on every host" has to mean it in the same sequence everywhere, or the
// result depends on which machine happened to answer first.

#ifndef OPENCRT_UI_WORKSPACE_COMMAND_WINDOW_H_
#define OPENCRT_UI_WORKSPACE_COMMAND_WINDOW_H_

#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QLineEdit;

namespace openscrt::ui {

class CommandWindow : public QWidget {
    Q_OBJECT

 public:
    // A session that can be written to.
    using Sink = std::function<void(const std::vector<std::uint8_t>&)>;

    explicit CommandWindow(QWidget* parent = nullptr);

    // The targets of a broadcast, in the order they are reached.
    void setBroadcastSinks(std::vector<Sink> sinks);
    // The target of a command that is not a broadcast.
    void setCurrentSink(Sink sink);

    [[nodiscard]] bool isBroadcast() const noexcept;
    void setBroadcast(bool on);

    [[nodiscard]] QString text() const;
    void setText(const QString& text);

    // Sends what is in the line edit and clears it. Returns how many sessions got it.
    //
    // The line is sent with a trailing newline, because a shell waits for one.
    std::size_t send();

 signals:
    // The command that was sent, whether it was a broadcast, and how many sessions got it.
    void commandSent(const QString& text, bool broadcast, std::size_t recipients);

 private:
    [[nodiscard]] std::vector<std::uint8_t> payload() const;

    QLineEdit* input_{nullptr};
    QCheckBox* broadcast_{nullptr};
    std::vector<Sink> broadcast_sinks_;
    Sink current_sink_;
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_COMMAND_WINDOW_H_
