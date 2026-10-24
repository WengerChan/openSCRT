#include "ui/workspace/ButtonBar.h"

#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <utility>

namespace openscrt::ui {
namespace {

// The dialog that asks for the three things a button has: what it is called, what it
// does, and the text it works with.
class ButtonEditorDialog : public QDialog {
 public:
    ButtonEditorDialog(const config::ButtonAction& action, QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowTitle(tr("Button"));

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(tr("Caption:"), this));
        label_edit_ = new QLineEdit(QString::fromStdString(action.label), this);
        layout->addWidget(label_edit_);

        layout->addWidget(new QLabel(tr("Action:"), this));
        type_box_ = new QComboBox(this);
        type_box_->addItem(tr("Send string"),
                           static_cast<int>(config::ButtonActionType::kSendString));
        type_box_->addItem(tr("Run script"),
                           static_cast<int>(config::ButtonActionType::kRunScript));
        type_box_->addItem(tr("Menu function"),
                           static_cast<int>(config::ButtonActionType::kMenuFunction));
        type_box_->addItem(tr("Open session"),
                           static_cast<int>(config::ButtonActionType::kOpenSession));
        const int index = type_box_->findData(static_cast<int>(action.type));
        type_box_->setCurrentIndex(index >= 0 ? index : 0);
        layout->addWidget(type_box_);

        layout->addWidget(new QLabel(tr("Value:"), this));
        value_edit_ = new QLineEdit(QString::fromStdString(action.value), this);
        layout->addWidget(value_edit_);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    [[nodiscard]] config::ButtonAction action() const {
        config::ButtonAction result;
        result.type = static_cast<config::ButtonActionType>(type_box_->currentData().toInt());
        result.label = label_edit_->text().toStdString();
        result.value = value_edit_->text().toStdString();
        return result;
    }

 private:
    QLineEdit* label_edit_{nullptr};
    QComboBox* type_box_{nullptr};
    QLineEdit* value_edit_{nullptr};
};

}  // namespace

std::vector<ButtonSendStep> expandButtonSendString(const QString& text, const QString& clipboard,
                                                   int pause_ms) {
    std::vector<ButtonSendStep> steps;
    QString pending;
    // The pause that comes before the next step.
    int pause = 0;

    const auto flush = [&steps, &pending, &pause]() {
        ButtonSendStep step;
        const QByteArray utf8 = pending.toUtf8();
        step.bytes.assign(utf8.constBegin(), utf8.constEnd());
        step.pause_ms = pause;
        steps.push_back(std::move(step));
        pending.clear();
        pause = 0;
    };

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch != QLatin1Char('\\')) {
            pending.append(ch);
            continue;
        }
        // A backslash at the very end is a backslash and nothing else.
        if (i + 1 >= text.size()) {
            pending.append(ch);
            break;
        }
        const QChar next = text.at(++i);
        if (next == QLatin1Char('r')) {
            pending.append(QLatin1Char('\r'));
        } else if (next == QLatin1Char('n')) {
            pending.append(QLatin1Char('\n'));
        } else if (next == QLatin1Char('v')) {
            pending.append(clipboard);
        } else if (next == QLatin1Char('p')) {
            // The pause comes before the next thing that is sent, so what is written so
            // far has to go out first.
            flush();
            pause = pause_ms;
        } else if (next == QLatin1Char('\\')) {
            pending.append(QLatin1Char('\\'));
        } else {
            // An escape this build does not know is passed on as it was typed.
            pending.append(ch);
            pending.append(next);
        }
    }
    if (!pending.isEmpty() || steps.empty()) {
        flush();
    }
    return steps;
}

ButtonBar::ButtonBar(QWidget* parent) : QWidget(parent) {
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(false);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs_);

    step_timer_ = new QTimer(this);
    step_timer_->setSingleShot(true);
    connect(step_timer_, &QTimer::timeout, this, &ButtonBar::sendNextStep);

    connect(
        tabs_->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint& point) {
            const int index = tabs_->tabBar()->tabAt(point);
            if (index < 0) {
                return;
            }
            QMenu menu(this);
            menu.addAction(tr("Add Group..."), this, [this]() { addGroup(); });
            menu.addAction(tr("Rename Group..."), this, [this, index]() { renameGroup(index); });
            menu.addAction(tr("Remove Group"), this, [this, index]() { removeGroup(index); });
            menu.exec(tabs_->tabBar()->mapToGlobal(point));
        });
}

void ButtonBar::setModel(const config::ButtonBarModel& model) {
    model_ = model;
    rebuild();
}

int ButtonBar::groupCount() const {
    return static_cast<int>(model_.groups.size());
}

int ButtonBar::currentGroup() const {
    return tabs_->currentIndex();
}

void ButtonBar::setCurrentGroup(int index) {
    if (index >= 0 && index < tabs_->count()) {
        tabs_->setCurrentIndex(index);
    }
}

QString ButtonBar::buttonText(int group, int button) const {
    const config::ButtonAction action = buttonAction(group, button);
    if (action.label.empty()) {
        return QString::fromStdString(action.value);
    }
    return QString::fromStdString(action.label);
}

config::ButtonAction ButtonBar::buttonAction(int group, int button) const {
    if (group < 0 || static_cast<std::size_t>(group) >= model_.groups.size()) {
        return {};
    }
    const auto& buttons = model_.groups[static_cast<std::size_t>(group)].buttons;
    if (button < 0 || static_cast<std::size_t>(button) >= buttons.size()) {
        return {};
    }
    return buttons[static_cast<std::size_t>(button)];
}

void ButtonBar::rebuild() {
    // The pages are rebuilt from scratch: the model is small, and a diff would have to
    // keep the tab order, the button order and the widget tree in step with each other.
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }

    for (std::size_t g = 0; g < model_.groups.size(); ++g) {
        auto* page = new QWidget(tabs_);
        auto* row = new QHBoxLayout(page);
        row->setContentsMargins(2, 2, 2, 2);
        row->setSpacing(2);

        // The empty space of a group is where a button is added.
        page->setContextMenuPolicy(Qt::CustomContextMenu);
        const int group_index = static_cast<int>(g);
        connect(page, &QWidget::customContextMenuRequested, this,
                [this, group_index, page](const QPoint& point) {
                    QMenu menu(this);
                    menu.addAction(tr("Add Button..."), this,
                                   [this, group_index]() { addButton(group_index); });
                    menu.exec(page->mapToGlobal(point));
                });

        const auto& buttons = model_.groups[g].buttons;
        for (std::size_t b = 0; b < buttons.size(); ++b) {
            const config::ButtonAction action = buttons[b];
            auto* button = new QToolButton(page);
            button->setText(action.label.empty() ? QString::fromStdString(action.value)
                                                 : QString::fromStdString(action.label));
            button->setToolTip(QString::fromStdString(action.value));
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            const int button_index = static_cast<int>(b);
            connect(button, &QToolButton::clicked, this, [this, group_index, button_index]() {
                onButtonPressed(group_index, button_index);
            });
            connect(
                button, &QWidget::customContextMenuRequested, this,
                [this, group_index, button_index, button](const QPoint& point) {
                    QMenu menu(this);
                    menu.addAction(tr("Edit Button..."), this, [this, group_index, button_index]() {
                        editButton(group_index, button_index);
                    });
                    menu.addAction(tr("Remove Button"), this, [this, group_index, button_index]() {
                        removeButton(group_index, button_index);
                    });
                    menu.exec(button->mapToGlobal(point));
                });
            row->addWidget(button);
        }
        row->addStretch(1);
        tabs_->addTab(page, QString::fromStdString(model_.groups[g].name));
    }
}

bool ButtonBar::trigger(int group, int button) {
    if (group < 0 || static_cast<std::size_t>(group) >= model_.groups.size()) {
        return false;
    }
    if (button < 0 || static_cast<std::size_t>(button) >=
                          model_.groups[static_cast<std::size_t>(group)].buttons.size()) {
        return false;
    }
    onButtonPressed(group, button);
    return true;
}

void ButtonBar::onButtonPressed(int group, int button) {
    const config::ButtonAction action = buttonAction(group, button);
    switch (action.type) {
        case config::ButtonActionType::kSendString: {
            const QString text = QString::fromStdString(action.value);
            emit sendStringRequested(text);
            // The clipboard is read here and not at the moment the button was pressed:
            // a `\v` pastes what is on the clipboard now.
            const QClipboard* clipboard = QGuiApplication::clipboard();
            const QString pasted = clipboard != nullptr ? clipboard->text() : QString();
            sendSteps(expandButtonSendString(text, pasted, pause_ms_));
            break;
        }
        case config::ButtonActionType::kRunScript:
            emit scriptRequested(QString::fromStdString(action.value));
            break;
        case config::ButtonActionType::kMenuFunction:
            emit menuFunctionRequested(QString::fromStdString(action.value));
            break;
        case config::ButtonActionType::kOpenSession:
            emit openSessionRequested(QString::fromStdString(action.value));
            break;
    }
}

void ButtonBar::sendSteps(std::vector<ButtonSendStep> steps) {
    pending_steps_ = std::move(steps);
    sendNextStep();
}

void ButtonBar::sendNextStep() {
    if (pending_steps_.empty()) {
        return;
    }
    const ButtonSendStep step = pending_steps_.front();
    pending_steps_.erase(pending_steps_.begin());
    if (sender_ && !step.bytes.empty()) {
        sender_(step.bytes.data(), step.bytes.size());
    }
    if (pending_steps_.empty()) {
        return;
    }
    // The pause of the next step is the wait between the two.
    const int pause = pending_steps_.front().pause_ms;
    if (pause > 0) {
        step_timer_->start(pause);
    } else {
        sendNextStep();
    }
}

void ButtonBar::addGroup() {
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New Group"), tr("Name of the group:"),
                                               QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.isEmpty()) {
        return;
    }
    model_.addGroup(name.toStdString());
    rebuild();
    emit modelChanged();
}

void ButtonBar::renameGroup(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= model_.groups.size()) {
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Group"), tr("Name of the group:"), QLineEdit::Normal,
        QString::fromStdString(model_.groups[static_cast<std::size_t>(index)].name), &accepted);
    if (!accepted || name.isEmpty()) {
        return;
    }
    model_.groups[static_cast<std::size_t>(index)].name = name.toStdString();
    rebuild();
    emit modelChanged();
}

void ButtonBar::removeGroup(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= model_.groups.size()) {
        return;
    }
    if (!model_.removeGroup(static_cast<std::size_t>(index))) {
        return;
    }
    rebuild();
    emit modelChanged();
}

void ButtonBar::addButton(int group) {
    if (group < 0 || static_cast<std::size_t>(group) >= model_.groups.size()) {
        return;
    }
    ButtonEditorDialog dialog(config::ButtonAction{}, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    model_.addButton(static_cast<std::size_t>(group), dialog.action());
    rebuild();
    emit modelChanged();
}

void ButtonBar::editButton(int group, int button) {
    const config::ButtonAction action = buttonAction(group, button);
    if (group < 0 || static_cast<std::size_t>(group) >= model_.groups.size()) {
        return;
    }
    if (button < 0 || static_cast<std::size_t>(button) >=
                          model_.groups[static_cast<std::size_t>(group)].buttons.size()) {
        return;
    }
    ButtonEditorDialog dialog(action, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    model_.groups[static_cast<std::size_t>(group)].buttons[static_cast<std::size_t>(button)] =
        dialog.action();
    rebuild();
    emit modelChanged();
}

void ButtonBar::removeButton(int group, int button) {
    if (group < 0 || static_cast<std::size_t>(group) >= model_.groups.size()) {
        return;
    }
    if (!model_.removeButton(static_cast<std::size_t>(group), static_cast<std::size_t>(button))) {
        return;
    }
    rebuild();
    emit modelChanged();
}

}  // namespace openscrt::ui
