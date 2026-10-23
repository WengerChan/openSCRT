#include "ui/workspace/SessionManagerWidget.h"

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>
#include <unordered_map>

namespace openscrt::ui {
namespace {

constexpr int kIdRole = Qt::UserRole;

}  // namespace

SessionManagerWidget::SessionManagerWidget(QWidget* parent) : QWidget(parent) {
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Session"), QStringLiteral("Host")});
    tree_->setRootIsDecorated(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);

    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) {
            return;
        }
        const QString id = item->data(0, kIdRole).toString();
        if (!id.isEmpty()) {
            emit sessionActivated(id);
        }
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            [this]() { emit sessionSelected(QString::fromStdString(selectedSessionId())); });
}

void SessionManagerWidget::setTree(const config::SessionTree& tree) {
    tree_->clear();

    // The items are created first and attached afterwards. Building them top-down would
    // require the parent to exist before the child, and a saved file is free to list a
    // child before its parent.
    std::unordered_map<std::string, QTreeWidgetItem*> items;
    items.reserve(tree.profiles().size());
    for (const config::SessionProfile& profile : tree.profiles()) {
        auto* item = new QTreeWidgetItem();
        const std::string label = profile.name.empty() ? profile.id : profile.name;
        item->setText(0, QString::fromStdString(label));
        item->setText(1, QString::fromStdString(profile.host.value_or("")));
        item->setData(0, kIdRole, QString::fromStdString(profile.id));
        items.emplace(profile.id, item);
    }

    for (const config::SessionProfile& profile : tree.profiles()) {
        auto* item = items.at(profile.id);
        const auto parent = items.find(profile.parent_id);
        if (!profile.parent_id.empty() && parent != items.end()) {
            parent->second->addChild(item);
        } else {
            tree_->addTopLevelItem(item);
        }
    }
    tree_->expandAll();
}

std::vector<std::string> SessionManagerWidget::sessionIds() const {
    std::vector<std::string> ids;
    const std::function<void(const QTreeWidgetItem*)> walk = [&](const QTreeWidgetItem* item) {
        ids.push_back(item->data(0, kIdRole).toString().toStdString());
        for (int index = 0; index < item->childCount(); ++index) {
            walk(item->child(index));
        }
    };
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        walk(tree_->topLevelItem(index));
    }
    return ids;
}

std::string SessionManagerWidget::selectedSessionId() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) {
        return {};
    }
    return item->data(0, kIdRole).toString().toStdString();
}

bool SessionManagerWidget::selectSession(const std::string& id) {
    const std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find =
        [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (item->data(0, kIdRole).toString().toStdString() == id) {
            return item;
        }
        for (int index = 0; index < item->childCount(); ++index) {
            if (QTreeWidgetItem* hit = find(item->child(index))) {
                return hit;
            }
        }
        return nullptr;
    };

    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = tree_->topLevelItem(index);
        if (QTreeWidgetItem* hit = find(item)) {
            tree_->setCurrentItem(hit);
            return true;
        }
    }
    return false;
}

}  // namespace openscrt::ui
