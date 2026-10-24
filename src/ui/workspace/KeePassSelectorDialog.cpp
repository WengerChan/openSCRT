#include "ui/workspace/KeePassSelectorDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace openscrt::ui {
namespace {

constexpr int kUuidRole = Qt::UserRole + 1;

}  // namespace

KeePassSelectorDialog::KeePassSelectorDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Choose a KeePass entry"));
    setObjectName(QStringLiteral("keepassSelectorDialog"));

    auto* layout = new QVBoxLayout(this);

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("keepassSelectorFilter"));
    filter_->setPlaceholderText(tr("Filter"));
    layout->addWidget(filter_);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("keepassSelectorTree"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Title"), tr("User"), tr("Group")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->header()->setStretchLastSection(true);
    layout->addWidget(tree_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(filter_, &QLineEdit::textChanged, this, &KeePassSelectorDialog::rebuild);
}

void KeePassSelectorDialog::setEntries(const credential::kdbx::KdbxIndex& index) {
    index_ = index;
    rebuild();
}

void KeePassSelectorDialog::setFilter(const QString& text) {
    filter_->setText(text);
}

void KeePassSelectorDialog::rebuild() {
    const QString needle = filter_->text().trimmed();
    tree_->clear();

    for (const credential::kdbx::KdbxEntry& entry : index_.entries) {
        const QString title = QString::fromStdString(entry.title);
        const QString user = QString::fromStdString(entry.username);
        const QString group = QString::fromStdString(entry.group_path);
        // QString::contains, not std::string::contains: the latter is C++23 and this
        // project is C++20.
        if (!needle.isEmpty() && !title.contains(needle, Qt::CaseInsensitive) &&
            !user.contains(needle, Qt::CaseInsensitive) &&
            !group.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, title);
        item->setText(1, user);
        item->setText(2, group);
        item->setData(0, kUuidRole, QString::fromStdString(entry.uuid));
    }
    if (tree_->topLevelItemCount() > 0) {
        tree_->setCurrentItem(tree_->topLevelItem(0));
    }
}

QString KeePassSelectorDialog::selectedEntryUuid() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) {
        return {};
    }
    return item->data(0, kUuidRole).toString();
}

}  // namespace openscrt::ui
