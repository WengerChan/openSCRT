#include "ui/workspace/WorkspaceTabs.h"

#include <utility>

#include "ui/workspace/TerminalCanvasWidget.h"
#include "ui/workspace/TerminalSession.h"

namespace openscrt::ui {

WorkspaceTabs::WorkspaceTabs(QWidget* parent) : QTabWidget(parent) {
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);
    connect(this, &QTabWidget::tabCloseRequested, this, &WorkspaceTabs::handleTabCloseRequested);
}

int WorkspaceTabs::openSession(const config::SessionProfile& profile, std::string& error) {
    auto* canvas = new TerminalCanvasWidget(this);
    // No QObject parent: the vector owns the session. A QObject that is both owned by a
    // unique_ptr and parented to a widget is deleted twice -- once by the vector, once by
    // the parent's child list.
    auto session = std::make_unique<TerminalSession>(profile, canvas);
    TerminalSession* raw = session.get();
    sessions_.push_back(std::move(session));

    connect(raw, &TerminalSession::stateChanged, this,
            [this, raw](int state, const QString& detail) {
                emit sessionStateChanged(raw->id(), state, detail);
            });

    const int index = addTab(canvas, raw->title());
    canvas->setFocus();

    if (!raw->connectSession(error)) {
        // The protocol has no implementation yet. A tab that can never connect is worse
        // than no tab at all, so it is removed again and the reason is handed back.
        removeTab(index);
        canvas->deleteLater();
        sessions_.pop_back();
        return -1;
    }

    emit sessionOpened(raw->id(), raw->title());
    return index;
}

int WorkspaceTabs::sessionCount() const {
    return static_cast<int>(sessions_.size());
}

QString WorkspaceTabs::activeSessionId() const {
    QWidget* page = currentWidget();
    if (page == nullptr) {
        return {};
    }
    for (const auto& session : sessions_) {
        if (session->canvas() == page) {
            return session->id();
        }
    }
    return {};
}

TerminalSession* WorkspaceTabs::sessionById(const QString& id) const {
    for (const auto& session : sessions_) {
        if (session->id() == id) {
            return session.get();
        }
    }
    return nullptr;
}

bool WorkspaceTabs::closeSession(const QString& id) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if ((*it)->id() != id) {
            continue;
        }
        TerminalSession* session = it->get();
        const int index = indexOf(session->canvas());
        // The transport stops before the canvas goes: the reader thread is what feeds it.
        session->close();
        if (index >= 0) {
            QWidget* canvas = session->canvas();
            removeTab(index);
            canvas->deleteLater();
        }
        sessions_.erase(it);
        emit sessionClosed(id);
        return true;
    }
    return false;
}

void WorkspaceTabs::closeAll() {
    while (!sessions_.empty()) {
        if (!closeSession(sessions_.front()->id())) {
            break;
        }
    }
}

void WorkspaceTabs::handleTabCloseRequested(int index) {
    QWidget* page = widget(index);
    if (page == nullptr) {
        return;
    }
    for (const auto& session : sessions_) {
        if (session->canvas() == page) {
            closeSession(session->id());
            return;
        }
    }
}

}  // namespace openscrt::ui
