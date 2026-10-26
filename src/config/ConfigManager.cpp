#include "config/ConfigManager.h"

#include <QSettings>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <utility>
#include <vector>

namespace openscrt::config {
namespace {

constexpr const char* kSessionsGroup = "sessions";
constexpr const char* kTemplateGroup = "defaultSession";
constexpr const char* kGeometryGroup = "window";
constexpr const char* kButtonBarGroup = "buttonbar";

QString sessionKey(const std::string& id, const char* field) {
    return QString::fromUtf8(kSessionsGroup) + QLatin1Char('/') + QString::fromStdString(id) +
           QLatin1Char('/') + QLatin1String(field);
}

QString templateKey(const char* field) {
    return QString::fromUtf8(kTemplateGroup) + QLatin1Char('/') + QLatin1String(field);
}

void writeProfile(QSettings& settings, const SessionProfile& profile, bool is_template) {
    const auto put = [&settings, &profile, is_template](const char* field, const QString& value) {
        if (is_template) {
            settings.setValue(templateKey(field), value);
        } else {
            settings.setValue(sessionKey(profile.id, field), value);
        }
    };

    put("name", QString::fromStdString(profile.name));
    if (!is_template) {
        settings.setValue(sessionKey(profile.id, "parent"),
                          QString::fromStdString(profile.parent_id));
    }
    // Only overridden fields are written. An unset optional stays absent, which is what makes
    // the inheritance survive a round trip.
    if (profile.host) {
        put("host", QString::fromStdString(*profile.host));
    }
    if (profile.port) {
        put("port", QString::number(*profile.port));
    }
    if (profile.user) {
        put("user", QString::fromStdString(*profile.user));
    }
    if (profile.protocol) {
        put("protocol", QString::fromUtf8(toString(*profile.protocol)));
    }
    if (profile.private_key_path) {
        put("key", QString::fromStdString(*profile.private_key_path));
    }
    // The binding of the credential, not the credential itself.
    if (profile.credential_source) {
        put("cred-source", QString::fromStdString(*profile.credential_source));
    }
    if (profile.credential_entry_uuid) {
        put("cred-uuid", QString::fromStdString(*profile.credential_entry_uuid));
    }
    if (profile.credential_match_host) {
        put("cred-host", QString::fromStdString(*profile.credential_match_host));
    }
    // The serial port and its parameters (P3-T1). Like the credential binding, these
    // are the session's own and do not inherit.
    if (profile.serial_port) {
        put("serial-port", QString::fromStdString(*profile.serial_port));
    }
    if (profile.serial_baud_rate) {
        put("serial-baud", QString::number(*profile.serial_baud_rate));
    }
    if (profile.serial_data_bits) {
        put("serial-data", QString::fromStdString(*profile.serial_data_bits));
    }
    if (profile.serial_parity) {
        put("serial-parity", QString::fromStdString(*profile.serial_parity));
    }
    if (profile.serial_stop_bits) {
        put("serial-stop", QString::fromStdString(*profile.serial_stop_bits));
    }
    if (profile.serial_flow_control) {
        put("serial-flow", QString::fromStdString(*profile.serial_flow_control));
    }
}

std::string readString(QSettings& settings, const QString& key) {
    return settings.value(key).toString().toStdString();
}

}  // namespace

SessionTree ConfigManager::loadTree(const std::string& ini_path) {
    QSettings settings(QString::fromStdString(ini_path), QSettings::IniFormat);

    SessionProfile fallback;
    fallback.name = readString(settings, templateKey("name"));
    if (fallback.name.empty()) {
        fallback.name = "Default Session";
    }
    if (settings.contains(templateKey("host"))) {
        fallback.host = readString(settings, templateKey("host"));
    }
    if (settings.contains(templateKey("port"))) {
        fallback.port = settings.value(templateKey("port")).toInt();
    }
    if (settings.contains(templateKey("user"))) {
        fallback.user = readString(settings, templateKey("user"));
    }
    if (settings.contains(templateKey("protocol"))) {
        fallback.protocol = protocolFromString(readString(settings, templateKey("protocol")));
    }
    if (settings.contains(templateKey("key"))) {
        fallback.private_key_path = readString(settings, templateKey("key"));
    }

    SessionTree tree{std::move(fallback)};

    // Ids are discovered from the group list, then added parents-first: add() rejects an
    // unknown parent, and a file may list a child before its parent.
    //
    // Qt 6 dropped the prefix-taking overload of childGroups(), so the group is entered
    // first: inside "sessions", childGroups() reports the session ids. childKeys() would
    // not work -- it reports the *leaf* keys of the current group, and every key of this
    // file lives one level deeper.
    QStringList ids;
    settings.beginGroup(QString::fromUtf8(kSessionsGroup));
    ids = settings.childGroups();
    settings.endGroup();
    ids.sort();

    struct Pending {
        SessionProfile profile;
    };
    std::vector<Pending> pending;
    pending.reserve(static_cast<std::size_t>(ids.size()));

    for (const QString& id : ids) {
        SessionProfile profile;
        profile.id = id.toStdString();
        profile.name = readString(settings, sessionKey(profile.id, "name"));
        profile.parent_id = readString(settings, sessionKey(profile.id, "parent"));
        if (settings.contains(sessionKey(profile.id, "host"))) {
            profile.host = readString(settings, sessionKey(profile.id, "host"));
        }
        if (settings.contains(sessionKey(profile.id, "port"))) {
            profile.port = settings.value(sessionKey(profile.id, "port")).toInt();
        }
        if (settings.contains(sessionKey(profile.id, "user"))) {
            profile.user = readString(settings, sessionKey(profile.id, "user"));
        }
        if (settings.contains(sessionKey(profile.id, "protocol"))) {
            profile.protocol =
                protocolFromString(readString(settings, sessionKey(profile.id, "protocol")));
        }
        if (settings.contains(sessionKey(profile.id, "key"))) {
            profile.private_key_path = readString(settings, sessionKey(profile.id, "key"));
        }
        if (settings.contains(sessionKey(profile.id, "cred-source"))) {
            profile.credential_source = readString(settings, sessionKey(profile.id, "cred-source"));
        }
        if (settings.contains(sessionKey(profile.id, "cred-uuid"))) {
            profile.credential_entry_uuid =
                readString(settings, sessionKey(profile.id, "cred-uuid"));
        }
        if (settings.contains(sessionKey(profile.id, "cred-host"))) {
            profile.credential_match_host =
                readString(settings, sessionKey(profile.id, "cred-host"));
        }
        if (settings.contains(sessionKey(profile.id, "serial-port"))) {
            profile.serial_port = readString(settings, sessionKey(profile.id, "serial-port"));
        }
        if (settings.contains(sessionKey(profile.id, "serial-baud"))) {
            profile.serial_baud_rate =
                settings.value(sessionKey(profile.id, "serial-baud")).toInt();
        }
        if (settings.contains(sessionKey(profile.id, "serial-data"))) {
            profile.serial_data_bits = readString(settings, sessionKey(profile.id, "serial-data"));
        }
        if (settings.contains(sessionKey(profile.id, "serial-parity"))) {
            profile.serial_parity = readString(settings, sessionKey(profile.id, "serial-parity"));
        }
        if (settings.contains(sessionKey(profile.id, "serial-stop"))) {
            profile.serial_stop_bits = readString(settings, sessionKey(profile.id, "serial-stop"));
        }
        if (settings.contains(sessionKey(profile.id, "serial-flow"))) {
            profile.serial_flow_control =
                readString(settings, sessionKey(profile.id, "serial-flow"));
        }
        pending.push_back(Pending{std::move(profile)});
    }

    // Repeated passes so a chain of any depth loads, whatever order the file used.
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (auto it = pending.begin(); it != pending.end();) {
            const std::string& parent = it->profile.parent_id;
            if (!parent.empty() && tree.find(parent) == nullptr) {
                ++it;
                continue;  // Parent not loaded yet.
            }
            if (tree.add(std::move(it->profile))) {
                progressed = true;
            }
            it = pending.erase(it);
        }
    }
    // Anything left referenced a parent that does not exist; drop it rather than loading a
    // tree whose resolution depends on a node that is not there.
    return tree;
}

bool ConfigManager::saveTree(const SessionTree& tree, const std::string& ini_path) {
    QSettings settings(QString::fromStdString(ini_path), QSettings::IniFormat);
    // Only the two groups this manager owns are replaced. settings.clear() would also wipe
    // every other group in the file -- and the window geometry lives in the same INI, so
    // clearing would silently undo the "the two do not churn each other" promise the header
    // makes. A removed session must not linger either, which is why the whole group goes.
    settings.remove(QString::fromUtf8(kSessionsGroup));
    settings.remove(QString::fromUtf8(kTemplateGroup));

    writeProfile(settings, tree.defaultSession(), true);
    for (const SessionProfile& profile : tree.profiles()) {
        writeProfile(settings, profile, false);
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool ConfigManager::saveButtonBar(const ButtonBarModel& model, const std::string& ini_path) {
    QSettings settings(QString::fromStdString(ini_path), QSettings::IniFormat);
    // The whole group goes, so a button the user removed does not linger in the file.
    settings.remove(QString::fromUtf8(kButtonBarGroup));

    settings.beginGroup(QString::fromUtf8(kButtonBarGroup));
    settings.setValue(QStringLiteral("group-count"), static_cast<int>(model.groups.size()));
    for (std::size_t g = 0; g < model.groups.size(); ++g) {
        const ButtonGroup& group = model.groups[g];
        settings.beginGroup(QStringLiteral("group%1").arg(static_cast<qulonglong>(g)));
        settings.setValue(QStringLiteral("name"), QString::fromStdString(group.name));
        settings.setValue(QStringLiteral("button-count"), static_cast<int>(group.buttons.size()));
        for (std::size_t b = 0; b < group.buttons.size(); ++b) {
            const ButtonAction& action = group.buttons[b];
            settings.beginGroup(QStringLiteral("button%1").arg(static_cast<qulonglong>(b)));
            settings.setValue(QStringLiteral("type"), QString::fromUtf8(toString(action.type)));
            settings.setValue(QStringLiteral("label"), QString::fromStdString(action.label));
            settings.setValue(QStringLiteral("value"), QString::fromStdString(action.value));
            settings.endGroup();
        }
        settings.endGroup();
    }
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

ButtonBarModel ConfigManager::loadButtonBar(const std::string& ini_path) {
    QSettings settings(QString::fromStdString(ini_path), QSettings::IniFormat);
    ButtonBarModel model;
    settings.beginGroup(QString::fromUtf8(kButtonBarGroup));
    const int group_count = settings.value(QStringLiteral("group-count"), 0).toInt();
    for (int g = 0; g < group_count; ++g) {
        settings.beginGroup(QStringLiteral("group%1").arg(g));
        ButtonGroup group;
        group.name = readString(settings, QStringLiteral("name"));
        const int button_count = settings.value(QStringLiteral("button-count"), 0).toInt();
        for (int b = 0; b < button_count; ++b) {
            settings.beginGroup(QStringLiteral("button%1").arg(b));
            ButtonAction action;
            const std::optional<ButtonActionType> type =
                buttonActionTypeFromString(readString(settings, QStringLiteral("type")));
            if (type.has_value()) {
                action.type = *type;
            }
            action.label = readString(settings, QStringLiteral("label"));
            action.value = readString(settings, QStringLiteral("value"));
            group.buttons.push_back(std::move(action));
            settings.endGroup();
        }
        settings.endGroup();
        model.groups.push_back(std::move(group));
    }
    settings.endGroup();
    return model;
}

void ConfigManager::saveWindowGeometry(QSettings& settings, const std::string& blob) {
    settings.setValue(QString::fromUtf8(kGeometryGroup) + QLatin1String("/geometry"),
                      QString::fromStdString(blob));
}

std::string ConfigManager::loadWindowGeometry(QSettings& settings) {
    return settings.value(QString::fromUtf8(kGeometryGroup) + QLatin1String("/geometry"))
        .toString()
        .toStdString();
}

}  // namespace openscrt::config