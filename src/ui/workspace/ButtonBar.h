// The button bar (P2-T10).
//
// SecureCRT puts a row of buttons at the bottom of the terminal. Every button does one
// thing -- send a string, run a script, call a menu function, open a session -- and the
// row is grouped into tabs, so there can be more than one set of buttons.
//
// The bar is built by the user: the groups are right-clicked into being, the buttons are
// added to them, and the whole thing is saved with the sessions. The model of that lives
// in `config/ButtonBarModel.h`, because it is configuration and not decoration.
//
// A send string carries the SecureCRT escapes: `\r` is a carriage return, `\n` a line
// feed, `\p` a pause of a second, `\v` the text of the clipboard. The pause is a step of
// its own rather than a sleep: a sleep in the UI thread would freeze the window, and
// the user has to be able to type while the bar waits.

#ifndef OPENCRT_UI_WORKSPACE_BUTTON_BAR_H_
#define OPENCRT_UI_WORKSPACE_BUTTON_BAR_H_

#include <QString>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "config/ButtonBarModel.h"

class QTabWidget;
class QTimer;

namespace openscrt::ui {

// One piece of a send string: the bytes to send, and the pause that comes before them.
struct ButtonSendStep {
    std::vector<std::uint8_t> bytes;
    // The pause in milliseconds. Zero means "no pause".
    int pause_ms{0};
};

// Expands the escapes of a send string into the steps that are sent.
//
// `\r` carriage return, `\n` line feed, `\p` a pause, `\v` the clipboard, `\\` a
// backslash. An escape this build does not know is passed on as it was typed, and a
// backslash at the very end is a backslash.
[[nodiscard]] std::vector<ButtonSendStep> expandButtonSendString(const QString& text,
                                                                 const QString& clipboard,
                                                                 int pause_ms = 1000);

class ButtonBar : public QWidget {
    Q_OBJECT

 public:
    explicit ButtonBar(QWidget* parent = nullptr);

    // The buttons that are shown. The model is the truth; the widget follows it.
    void setModel(const config::ButtonBarModel& model);
    [[nodiscard]] const config::ButtonBarModel& model() const noexcept { return model_; }

    // Where the text goes. The callback is asked again for every step, so a button
    // follows the session that is in front when it is pressed.
    using Sender = std::function<void(const std::uint8_t* data, std::size_t size)>;
    void setSender(Sender sender) { sender_ = std::move(sender); }

    // How long a `\p` pauses. A test shortens it; the program keeps the SecureCRT second.
    void setPauseDuration(int milliseconds) { pause_ms_ = milliseconds; }
    [[nodiscard]] int pauseDuration() const noexcept { return pause_ms_; }

    [[nodiscard]] int groupCount() const;
    [[nodiscard]] int currentGroup() const;
    void setCurrentGroup(int index);

    // The caption of a button, and what it does.
    [[nodiscard]] QString buttonText(int group, int button) const;
    [[nodiscard]] config::ButtonAction buttonAction(int group, int button) const;

    // Presses a button. Returns false when there is no such button.
    bool trigger(int group, int button);

 signals:
    void sendStringRequested(const QString& text);
    void openSessionRequested(const QString& session_id);
    void scriptRequested(const QString& path);
    void menuFunctionRequested(const QString& path);
    // The user changed the bar, so it has to be saved.
    void modelChanged();

 private:
    void rebuild();
    void addGroup();
    void renameGroup(int index);
    void removeGroup(int index);
    void addButton(int group);
    void editButton(int group, int button);
    void removeButton(int group, int button);
    void onButtonPressed(int group, int button);
    void sendSteps(std::vector<ButtonSendStep> steps);
    void sendNextStep();

    config::ButtonBarModel model_;
    QTabWidget* tabs_{nullptr};
    QTimer* step_timer_{nullptr};
    Sender sender_;
    int pause_ms_{1000};
    // The steps that are still to be sent, and the one that is being waited for.
    std::vector<ButtonSendStep> pending_steps_;
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_BUTTON_BAR_H_
