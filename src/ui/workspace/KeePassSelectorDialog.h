// The KeePass entry picker (P2-T9).
//
// The dialog a user gets when a session is bound to "ask me every time", or when they
// want to bind a session to an entry by hand. It lists the entries of the database and
// hands back the uuid of the one that was picked.
//
// It shows the title, the user name and the group of every entry, and never the
// password: the password comes out of the database on demand, in a locked buffer, and
// this dialog has no business holding one.

#ifndef OPENCRT_UI_WORKSPACE_KEEPASS_SELECTOR_DIALOG_H_
#define OPENCRT_UI_WORKSPACE_KEEPASS_SELECTOR_DIALOG_H_

#include <QDialog>
#include <QString>

#include "credential/kdbx/KdbxXmlIndex.h"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace openscrt::ui {

class KeePassSelectorDialog : public QDialog {
    Q_OBJECT

 public:
    explicit KeePassSelectorDialog(QWidget* parent = nullptr);

    // Fills the list. The entries keep the order of the index, and the first one is
    // selected.
    void setEntries(const credential::kdbx::KdbxIndex& index);

    // The uuid of the entry the user picked, or an empty string when the dialog was
    // cancelled.
    [[nodiscard]] QString selectedEntryUuid() const;

    // Filters the list as the user types. An empty filter shows everything.
    void setFilter(const QString& text);

 private:
    void rebuild();

    QTreeWidget* tree_{nullptr};
    QLineEdit* filter_{nullptr};
    credential::kdbx::KdbxIndex index_;
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_KEEPASS_SELECTOR_DIALOG_H_
