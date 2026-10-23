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

QString sessionKey(const std::string& id, const char* field) {
    return QString::fromUtf8(kSessionsGroup) + QLatin1Char('/') + QString::fromStdString(id) +
           QLatin1Char('/') + QLatin1String(field);
}

QString templateKey(const char* field) {
    return QString::fromUtf8(kTemplateGroup) + QLatin1Char('/') + QLatin1String(field);
}

void writeProfile(QSettings& settings, const SessionProfile& profile, bool is_template) {
    const auto put = [&settings, is_template](const char* field, const QString& value) {
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
        put("port", *profile.port);
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
    QStringList ids = settings.childGroups(QString::fromUtf8(kSessionsGroup));
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