#include "ui/workspace/WorkspacePanes.h"

#include <QApplication>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "ui/workspace/TerminalSession.h"
#include "ui/workspace/WorkspaceTabs.h"

namespace openscrt::ui {

WorkspacePanes::WorkspacePanes(QWidget* parent) : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);

    root_ = new QSplitter(Qt::Horizontal, this);
    root_->setChildrenCollapsible(false);
    layout_->addWidget(root_);

    WorkspaceTabs* first = createPane();
    root_->addWidget(first);
    active_pane_ = first;

    // The pane the user is working in is the one that holds the keyboard focus. The
    // focus is watched instead of asked for on every call, because asking would make
    // "which pane is active" depend on whether a hidden widget can hold the focus --
    // and that differs between a shown window and a test.
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app != nullptr) {
        connect(app, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (now == nullptr) {
                return;
            }
            for (WorkspaceTabs* pane : panes_) {
                if (pane->isAncestorOf(now)) {
                    active_pane_ = pane;
                    return;
                }
            }
        });
    }
}

WorkspacePanes::~WorkspacePanes() = default;

WorkspaceTabs* WorkspacePanes::createPane() {
    auto* pane = new WorkspaceTabs();
    panes_.push_back(pane);
    connectPane(pane);
    return pane;
}

void WorkspacePanes::connectPane(WorkspaceTabs* pane) {
    connect(pane, &WorkspaceTabs::sessionOpened, this,
            [this, pane](const QString& id, const QString& title) {
                onPaneSessionOpened(id, title, pane);
            });
    connect(pane, &WorkspaceTabs::sessionClosed, this,
            [this](const QString& id) { onPaneSessionClosed(id); });
    connect(pane, &WorkspaceTabs::sessionStateChanged, this, &WorkspacePanes::sessionStateChanged);
    // A tab that becomes current is the pane the user is working in.
    connect(pane, &WorkspaceTabs::currentChanged, this, [this, pane](int) { active_pane_ = pane; });
}

void WorkspacePanes::onPaneSessionOpened(const QString& id, const QString& title,
                                         WorkspaceTabs* pane) {
    SessionRef ref;
    ref.id = id;
    ref.session = pane->sessionById(id);
    order_.push_back(ref);
    active_pane_ = pane;
    emit sessionOpened(id, title);
}

void WorkspacePanes::onPaneSessionClosed(const QString& id) {
    // The session is already gone when this signal arrives, so the entry is found by
    // name and the pointer next to it is dropped without being read.
    forgetSession(id);
    emit sessionClosed(id);
}

void WorkspacePanes::forgetSession(const QString& id) {
    for (auto it = order_.begin(); it != order_.end(); ++it) {
        if (it->id == id) {
            order_.erase(it);
            return;
        }
    }
}

int WorkspacePanes::openSession(const config::SessionProfile& profile, std::string& error,
                                core::SecureByteArray password) {
    WorkspaceTabs* pane = activePane();
    if (pane == nullptr) {
        pane = createPane();
        root_->addWidget(pane);
        active_pane_ = pane;
    }
    return pane->openSession(profile, error, std::move(password));
}

int WorkspacePanes::sessionCount() const {
    return static_cast<int>(order_.size());
}

QString WorkspacePanes::activeSessionId() const {
    WorkspaceTabs* pane = activePane();
    return pane == nullptr ? QString{} : pane->activeSessionId();
}

TerminalSession* WorkspacePanes::sessionById(const QString& id) const {
    for (const SessionRef& ref : order_) {
        if (ref.id == id && ref.session != nullptr) {
            return ref.session;
        }
    }
    return nullptr;
}

QStringList WorkspacePanes::sessionTitles() const {
    QStringList titles;
    for (const SessionRef& ref : order_) {
        if (ref.session != nullptr) {
            titles << ref.session->title();
        }
    }
    return titles;
}

bool WorkspacePanes::closeSession(const QString& id) {
    for (WorkspaceTabs* pane : panes_) {
        if (pane->sessionById(id) != nullptr) {
            return pane->closeSession(id);
        }
    }
    return false;
}

void WorkspacePanes::closeAll() {
    for (WorkspaceTabs* pane : panes_) {
        pane->closeAll();
    }
}

WorkspaceTabs* WorkspacePanes::paneAt(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= panes_.size()) {
        return nullptr;
    }
    return panes_[static_cast<std::size_t>(index)];
}

int WorkspacePanes::paneCount() const {
    return static_cast<int>(panes_.size());
}

WorkspaceTabs* WorkspacePanes::activePane() const {
    // The pane that was used last: a session was opened in it, its tab was switched, or
    // the focus is in it. A split makes the new pane the active one, so the session
    // that is opened next lands in the pane the user just made.
    if (active_pane_ != nullptr) {
        return active_pane_;
    }
    return panes_.empty() ? nullptr : panes_.front();
}

void WorkspacePanes::split(Qt::Orientation orientation) {
    WorkspaceTabs* active = activePane();
    if (active == nullptr) {
        return;
    }
    WorkspaceTabs* fresh = createPane();
    // The new pane is the active one: the user made it to type into it.
    active_pane_ = fresh;

    auto* parent = qobject_cast<QSplitter*>(active->parentWidget());
    if (parent != nullptr && parent->orientation() == orientation) {
        // The parent already runs the way we want to split: the new pane goes right
        // next to the active one.
        parent->insertWidget(parent->indexOf(active) + 1, fresh);
        return;
    }

    // The parent runs the other way, so the two panes go into a splitter of their own
    // that takes the place the active pane had.
    const bool active_was_hidden = active->isHidden();
    auto* nested = new QSplitter(orientation, this);
    const int index = (parent != nullptr) ? parent->indexOf(active) : 0;
    nested->addWidget(active);
    nested->addWidget(fresh);
    if (parent != nullptr) {
        parent->insertWidget(index, nested);
    } else {
        root_->addWidget(nested);
    }
    // Moving a widget to a new parent hides it, and a pane that disappeared from the
    // screen would look like the split had eaten it.
    active->setHidden(active_was_hidden);
}

void WorkspacePanes::tile() {
    while (panes_.size() < 4) {
        WorkspaceTabs* fresh = createPane();
        root_->addWidget(fresh);
    }

    // The panes move, the sessions stay with them: the new tree is built around the
    // same four panes, and the old tree is thrown away once it is empty.
    std::vector<bool> was_hidden;
    was_hidden.reserve(panes_.size());
    for (WorkspaceTabs* pane : panes_) {
        was_hidden.push_back(pane->isHidden());
    }

    auto* new_root = new QSplitter(Qt::Horizontal, this);
    new_root->setChildrenCollapsible(false);
    auto* left = new QSplitter(Qt::Vertical, new_root);
    auto* right = new QSplitter(Qt::Vertical, new_root);
    new_root->addWidget(left);
    new_root->addWidget(right);
    // Adding a pane to a splitter moves it there, which takes it out of the old tree.
    left->addWidget(panes_[0]);
    left->addWidget(panes_[1]);
    right->addWidget(panes_[2]);
    right->addWidget(panes_[3]);
    // Panes that do not fit in the grid join the right column. Every pane has to end up
    // in the new tree, because the old one is thrown away and a pane left behind in it
    // would be deleted with its sessions still in it.
    for (std::size_t i = 4; i < panes_.size(); ++i) {
        right->addWidget(panes_[i]);
    }

    delete root_;
    root_ = new_root;
    layout_->addWidget(root_);

    for (std::size_t i = 0; i < panes_.size(); ++i) {
        panes_[i]->setHidden(was_hidden[i]);
    }
}

void WorkspacePanes::closeActivePane() {
    WorkspaceTabs* pane = activePane();
    if (pane == nullptr) {
        return;
    }

    // The sessions go first. The pane owns them, and a session that is still running
    // when its canvas disappears is a thread that outlives its widget.
    pane->closeAll();

    if (qobject_cast<QSplitter*>(pane->parentWidget()) != nullptr) {
        // QSplitter has no removeWidget, and replaceWidget refuses a null widget -- it
        // answers "QSplitter::replaceWidget: Widget can't be null" and keeps the old
        // one. So the pane is taken out of the splitter by reparenting it to nothing:
        // the splitter then drops the handle and the slot with it. A pane that was only
        // removed from the list would stay in the tree, and the layout would keep
        // giving it space.
        pane->setParent(nullptr);
    }
    panes_.erase(std::remove(panes_.begin(), panes_.end(), pane), panes_.end());
    if (active_pane_ == pane) {
        active_pane_ = panes_.empty() ? nullptr : panes_.front();
    }
    pane->deleteLater();

    // There is always at least one pane to work in.
    if (panes_.empty()) {
        WorkspaceTabs* fresh = createPane();
        root_->addWidget(fresh);
        active_pane_ = fresh;
    }
}

void WorkspacePanes::nextTab() {
    WorkspaceTabs* pane = activePane();
    if (pane == nullptr) {
        return;
    }
    const int index = pane->currentIndex();
    if (index >= 0 && index + 1 < pane->count()) {
        pane->setCurrentIndex(index + 1);
    }
}

void WorkspacePanes::previousTab() {
    WorkspaceTabs* pane = activePane();
    if (pane == nullptr) {
        return;
    }
    const int index = pane->currentIndex();
    if (index > 0) {
        pane->setCurrentIndex(index - 1);
    }
}

}  // namespace openscrt::ui
