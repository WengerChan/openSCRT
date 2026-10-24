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
        if (!needle.isEmpty() && !entry.title.contains(needle.toStdString(), Qt::CaseInsensitive) &&
            !entry.username.contains(needle.toStdString(), Qt::CaseInsensitive) &&
            !entry.group_path.contains(needle.toStdString(), Qt::CaseInsensitive)) {
            continue;
        }
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, QString::fromStdString(entry.title));
        item->setText(1, QString::fromStdString(entry.username));
        item->setText(2, QString::fromStdString(entry.group_path));
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
