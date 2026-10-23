// The Session Manager dock (P1-T12).
//
// Shows the saved session tree the way SecureCRT does: a folder-like hierarchy where a
// session can sit under a parent session and inherit its settings. The widget is a view --
// it never resolves anything itself, it just reports what the user picked.

#ifndef OPENCRT_UI_WORKSPACE_SESSION_MANAGER_WIDGET_H_
#define OPENCRT_UI_WORKSPACE_SESSION_MANAGER_WIDGET_H_

#include <QWidget>
#include <string>
#include <vector>

#include "config/SessionProfile.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace openscrt::ui {

class SessionManagerWidget : public QWidget {
    Q_OBJECT

 public:
    explicit SessionManagerWidget(QWidget* parent = nullptr);

    // Rebuilds the tree from the saved sessions.
    void setTree(const config::SessionTree& tree);

    // Every session id in the tree, parents before children.
    [[nodiscard]] std::vector<std::string> sessionIds() const;

    // The id of the selected session, or an empty string when nothing is selected.
    [[nodiscard]] std::string selectedSessionId() const;

    // Selects a session by id. Returns false when the id is not in the tree.
    bool selectSession(const std::string& id);

    [[nodiscard]] QTreeWidget* treeWidget() const noexcept { return tree_; }

 signals:
    void sessionActivated(const QString& id);
    void sessionSelected(const QString& id);

 private:
    QTreeWidget* tree_{nullptr};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_SESSION_MANAGER_WIDGET_H_
