// The split workspace (P2-T10).
//
// SecureCRT divides its terminal area into panes: every pane has its own tab strip and
// its own sessions, and the panes sit next to each other in a tree of splitters. This
// widget is that tree.
//
// The shape of the tree is the shape of the screen. Splitting to the right puts a new
// pane next to the pane that has the focus, and splitting downwards puts it under that
// one. When the splitter above runs the other way, the two panes go into a nested
// splitter, so a left column can be split again without disturbing the right one.
//
// Tile arranges four panes in a grid. It is not only a look: after a tile the sessions
// are still where they were, because the panes move, not the sessions.
//
// The sessions themselves stay in the panes. This widget only knows where they are and
// in which order they were opened -- and the order matters, because a broadcast that
// reaches twenty machines has to reach them in the same order every time.

#ifndef OPENCRT_UI_WORKSPACE_WORKSPACE_PANES_H_
#define OPENCRT_UI_WORKSPACE_WORKSPACE_PANES_H_

#include <QStringList>
#include <QWidget>
#include <string>
#include <vector>

#include "config/SessionProfile.h"
#include "core/SecureByteArray.h"

class QSplitter;
class QVBoxLayout;

namespace openscrt::ui {

class TerminalSession;
class WorkspaceTabs;

class WorkspacePanes : public QWidget {
    Q_OBJECT

 public:
    // A session of the workspace, remembered by name as well as by pointer: a session
    // that was closed is gone by the time its "closed" signal arrives, and the pointer
    // must not be touched then.
    struct SessionRef {
        QString id;
        TerminalSession* session{nullptr};
    };

    explicit WorkspacePanes(QWidget* parent = nullptr);
    ~WorkspacePanes() override;

    // Opens a session in the pane that has the focus. Returns the tab index inside that
    // pane, or -1 with `error` filled in when the protocol has no implementation yet.
    int openSession(const config::SessionProfile& profile, std::string& error,
                    core::SecureByteArray password = {});

    [[nodiscard]] int sessionCount() const;
    [[nodiscard]] QString activeSessionId() const;
    [[nodiscard]] TerminalSession* sessionById(const QString& id) const;

    // Every session, in the order they were opened. The order is what makes a broadcast
    // reproducible: twenty machines get the same command in the same order.
    [[nodiscard]] const std::vector<SessionRef>& sessions() const noexcept { return order_; }

    // The titles of every session, in the same order.
    [[nodiscard]] QStringList sessionTitles() const;

    bool closeSession(const QString& id);
    void closeAll();

    // The panes.
    [[nodiscard]] int paneCount() const;
    [[nodiscard]] WorkspaceTabs* paneAt(int index) const;
    // The pane that has the focus, or the one that was used last.
    [[nodiscard]] WorkspaceTabs* activePane() const;

    // Splits the pane that has the focus, to the right or below it.
    void split(Qt::Orientation orientation);
    // Arranges four panes in a 2x2 grid.
    void tile();
    // Closes the pane that has the focus, with the sessions in it.
    void closeActivePane();

    // Moves to the next or the previous tab of the pane that has the focus.
    void nextTab();
    void previousTab();

 signals:
    void sessionOpened(const QString& id, const QString& title);
    void sessionClosed(const QString& id);
    void sessionStateChanged(const QString& id, int state, const QString& detail);

 private:
    WorkspaceTabs* createPane();
    void connectPane(WorkspaceTabs* pane);
    void onPaneSessionOpened(const QString& id, const QString& title, WorkspaceTabs* pane);
    void onPaneSessionClosed(const QString& id);
    void forgetSession(const QString& id);

    QVBoxLayout* layout_{nullptr};
    QSplitter* root_{nullptr};
    std::vector<WorkspaceTabs*> panes_;
    // The sessions of every pane, in the order they were opened.
    std::vector<SessionRef> order_;
    WorkspaceTabs* active_pane_{nullptr};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_WORKSPACE_PANES_H_
