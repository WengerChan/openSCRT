// The tabbed workspace (P1-T12).
//
// One tab per session. The tab owns the canvas widget; the session object owns the
// transport behind it. Splitting and tiling the workspace into panes is Phase 2 work
// (P2-T10) -- Phase 1 has the tabbed half of the roadmap's "multi-mode workspace".

#ifndef OPENCRT_UI_WORKSPACE_WORKSPACE_TABS_H_
#define OPENCRT_UI_WORKSPACE_WORKSPACE_TABS_H_

#include <QTabWidget>
#include <memory>
#include <string>
#include <vector>

#include "config/SessionProfile.h"

namespace openscrt::ui {

class TerminalSession;

class WorkspaceTabs : public QTabWidget {
    Q_OBJECT

 public:
    explicit WorkspaceTabs(QWidget* parent = nullptr);

    // Opens a tab for the session and starts connecting it. Returns the tab index, or -1
    // with `error` filled in when the session's protocol has no implementation yet.
    int openSession(const config::SessionProfile& profile, std::string& error);

    [[nodiscard]] int sessionCount() const;
    [[nodiscard]] QString activeSessionId() const;
    [[nodiscard]] TerminalSession* sessionById(const QString& id) const;

    bool closeSession(const QString& id);
    void closeAll();

 signals:
    void sessionOpened(const QString& id, const QString& title);
    void sessionClosed(const QString& id);
    void sessionStateChanged(const QString& id, int state, const QString& detail);

 private:
    void handleTabCloseRequested(int index);

    std::vector<std::unique_ptr<TerminalSession>> sessions_;
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_WORKSPACE_TABS_H_
