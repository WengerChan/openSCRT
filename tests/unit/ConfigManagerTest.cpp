// Config persistence tests (P1-T12).
//
// The property that matters is not "the file round trips" but "the inheritance survives
// it". Saving resolved values would flatten the tree and silently break every later edit to
// the Default Session template, and no shape check on the file would reveal it.

#include <gtest/gtest.h>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <string>

#include "config/ConfigManager.h"

namespace openscrt::config {
namespace {

class ConfigManagerTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        path_ = std::string(QDir(dir_.path()).filePath("sessions.ini").toStdString());
    }

    static SessionProfile profile(std::string id, std::string parent = {}) {
        SessionProfile value;
        value.id = std::move(id);
        value.name = value.id;
        value.parent_id = std::move(parent);
        return value;
    }

    QTemporaryDir dir_;
    std::string path_;
};

TEST_F(ConfigManagerTest, AMissingFileYieldsTheBuiltInTemplate) {
    const SessionTree tree = ConfigManager::loadTree("/nonexistent/openscrt/never.ini");
    EXPECT_EQ(tree.defaultSession().name, "Default Session");
    EXPECT_EQ(tree.size(), 0u) << "a missing config must not invent sessions";

    // The Default Session template is the root of every chain and is always complete, so it
    // is read directly rather than through resolve(): resolve() looks a *session* up by id,
    // and the template is not one of the tree's sessions.
    EXPECT_EQ(tree.defaultSession().host.value_or(""), "localhost");
    EXPECT_EQ(tree.defaultSession().port.value_or(0), 22);
}

TEST_F(ConfigManagerTest, AnEmptyTreeRoundTrips) {
    const SessionTree original;
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    EXPECT_EQ(loaded.size(), 0u);
    EXPECT_EQ(loaded.defaultSession().host.value_or(""), "localhost");
    EXPECT_EQ(loaded.defaultSession().port.value_or(0), 22);
}

TEST_F(ConfigManagerTest, OverriddenFieldsSurviveARoundTrip) {
    auto group = profile("group");
    group.host = std::string("group.example");
    auto leaf = profile("leaf", "group");
    leaf.port = 2200;

    SessionTree original;
    ASSERT_TRUE(original.add(std::move(group)));
    ASSERT_TRUE(original.add(std::move(leaf)));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    SessionTree::Resolved resolved;
    ASSERT_TRUE(loaded.resolve("leaf", resolved));
    EXPECT_EQ(resolved.host, "group.example") << "still inherited after the round trip";
    EXPECT_EQ(resolved.port, 2200);
}

TEST_F(ConfigManagerTest, TheCredentialBindingSurvivesARoundTrip) {
    // The binding of a credential is configuration: the user picked it, so it has to
    // still be there after the session tree is written and read back.
    auto leaf = profile("leaf");
    leaf.credential_source = std::string("keepass-uuid");
    leaf.credential_entry_uuid = std::string("uuid-router");
    leaf.credential_match_host = std::string("10.20.30.40");

    SessionTree original;
    ASSERT_TRUE(original.add(std::move(leaf)));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    const SessionProfile* loaded_profile = loaded.find("leaf");
    ASSERT_NE(loaded_profile, nullptr);
    EXPECT_EQ(loaded_profile->credential_source, std::optional<std::string>("keepass-uuid"));
    EXPECT_EQ(loaded_profile->credential_entry_uuid, std::optional<std::string>("uuid-router"));
    EXPECT_EQ(loaded_profile->credential_match_host, std::optional<std::string>("10.20.30.40"));
}

TEST_F(ConfigManagerTest, TheSerialSettingsSurviveARoundTrip) {
    // The serial port and its parameters are configuration: a session that points at a
    // device has to still point at it after the tree is written and read back.
    auto leaf = profile("leaf");
    leaf.protocol = Protocol::kSerial;
    leaf.serial_port = std::string("/dev/ttyUSB0");
    leaf.serial_baud_rate = 115200;
    leaf.serial_data_bits = std::string("7");
    leaf.serial_parity = std::string("odd");
    leaf.serial_stop_bits = std::string("2");
    leaf.serial_flow_control = std::string("rts-cts");

    SessionTree original;
    ASSERT_TRUE(original.add(std::move(leaf)));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    const SessionProfile* loaded_profile = loaded.find("leaf");
    ASSERT_NE(loaded_profile, nullptr);
    EXPECT_EQ(loaded_profile->serial_port, std::optional<std::string>("/dev/ttyUSB0"));
    EXPECT_EQ(loaded_profile->serial_baud_rate, std::optional<int>(115200));
    EXPECT_EQ(loaded_profile->serial_data_bits, std::optional<std::string>("7"));
    EXPECT_EQ(loaded_profile->serial_parity, std::optional<std::string>("odd"));
    EXPECT_EQ(loaded_profile->serial_stop_bits, std::optional<std::string>("2"));
    EXPECT_EQ(loaded_profile->serial_flow_control, std::optional<std::string>("rts-cts"));
}

TEST_F(ConfigManagerTest, TheSerialSettingsDoNotComeFromTheParent) {
    // A child session usually sits on another device. Inheriting the parent's port
    // name would open the wrong one, so the serial settings are per session -- the
    // same rule the credential binding follows.
    auto parent = profile("parent");
    parent.serial_port = std::string("/dev/ttyUSB0");
    parent.serial_baud_rate = 115200;

    auto child = profile("child", "parent");

    SessionTree original;
    ASSERT_TRUE(original.add(std::move(parent)));
    ASSERT_TRUE(original.add(std::move(child)));

    const SessionProfile effective = original.effectiveProfile("child");
    EXPECT_FALSE(effective.serial_port.has_value());
    EXPECT_FALSE(effective.serial_baud_rate.has_value());
}

TEST_F(ConfigManagerTest, AFieldThatWasNeverOverriddenStaysInherited) {
    // The whole point: after a save/load cycle an inheriting session must still inherit,
    // otherwise a later template edit would stop reaching it.
    SessionTree original;
    ASSERT_TRUE(original.add(profile("alpha")));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    SessionTree loaded = ConfigManager::loadTree(path_);
    const SessionProfile* alpha = loaded.find("alpha");
    ASSERT_NE(alpha, nullptr);
    EXPECT_TRUE(alpha->inheritsEverything()) << "saving resolved values would have flattened it";

    SessionProfile updated = loaded.defaultSession();
    updated.host = std::string("moved.example");
    loaded.setDefaultSession(updated);

    SessionTree::Resolved resolved;
    ASSERT_TRUE(loaded.resolve("alpha", resolved));
    EXPECT_EQ(resolved.host, "moved.example") << "the template edit still reaches it";
}

TEST_F(ConfigManagerTest, ATemplateChangePersistsAndReachesInheritingSessions) {
    SessionTree original;
    ASSERT_TRUE(original.add(profile("alpha")));
    auto pinned = profile("pinned");
    pinned.host = std::string("pinned.example");
    ASSERT_TRUE(original.add(std::move(pinned)));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    SessionProfile updated = loaded.defaultSession();
    updated.host = std::string("changed.example");
    updated.port = 2202;
    SessionTree mutable_tree = loaded;
    mutable_tree.setDefaultSession(updated);
    ASSERT_TRUE(ConfigManager::saveTree(mutable_tree, path_));

    const SessionTree again = ConfigManager::loadTree(path_);
    SessionTree::Resolved inheriting;
    ASSERT_TRUE(again.resolve("alpha", inheriting));
    EXPECT_EQ(inheriting.host, "changed.example");
    EXPECT_EQ(inheriting.port, 2202);

    SessionTree::Resolved explicit_session;
    ASSERT_TRUE(again.resolve("pinned", explicit_session));
    EXPECT_EQ(explicit_session.host, "pinned.example") << "an override survives the template edit";
}

TEST_F(ConfigManagerTest, ParentsAndChildrenLoadRegardlessOfFileOrder) {
    SessionTree original;
    auto top = profile("top");
    top.host = std::string("top.example");
    ASSERT_TRUE(original.add(std::move(top)));
    ASSERT_TRUE(original.add(profile("mid", "top")));
    auto leaf = profile("leaf", "mid");
    leaf.port = 2201;
    ASSERT_TRUE(original.add(std::move(leaf)));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    EXPECT_EQ(loaded.size(), 3u) << "a chain of any depth must load";
    SessionTree::Resolved resolved;
    ASSERT_TRUE(loaded.resolve("leaf", resolved));
    EXPECT_EQ(resolved.host, "top.example");
    EXPECT_EQ(resolved.port, 2201);
}

TEST_F(ConfigManagerTest, AnEntryWithAMissingParentIsDroppedRatherThanBroken) {
    // Written by hand: the tree refuses to hold a profile whose parent does not exist, so
    // this shape can only come from a file that was edited by hand or written by another
    // tool. Saving the tree could never produce it, which is why the file is built here.
    {
        QSettings settings(QString::fromStdString(path_), QSettings::IniFormat);
        settings.setValue(QStringLiteral("sessions/orphan/name"), QStringLiteral("orphan"));
        settings.setValue(QStringLiteral("sessions/orphan/parent"),
                          QStringLiteral("does-not-exist"));
        settings.setValue(QStringLiteral("sessions/orphan/host"), QStringLiteral("orphan.example"));
        settings.sync();
    }

    const SessionTree loaded = ConfigManager::loadTree(path_);
    EXPECT_EQ(loaded.size(), 0u) << "an entry with a dangling parent must not be loaded";
    SessionTree::Resolved resolved;
    EXPECT_FALSE(loaded.resolve("orphan", resolved));
}

TEST_F(ConfigManagerTest, ASessionThatWasRemovedDoesNotComeBack) {
    // saveTree() replaces the group it owns, so a session that is gone from the tree must
    // not linger in the file: the next load would resurrect it.
    SessionTree original;
    ASSERT_TRUE(original.add(profile("kept")));
    ASSERT_TRUE(original.add(profile("dropped")));
    ASSERT_TRUE(ConfigManager::saveTree(original, path_));
    ASSERT_EQ(ConfigManager::loadTree(path_).size(), 2u);

    SessionTree reduced;
    ASSERT_TRUE(reduced.add(profile("kept")));
    ASSERT_TRUE(ConfigManager::saveTree(reduced, path_));

    const SessionTree loaded = ConfigManager::loadTree(path_);
    EXPECT_EQ(loaded.size(), 1u);
    EXPECT_NE(loaded.find("kept"), nullptr);
    EXPECT_EQ(loaded.find("dropped"), nullptr);
}

TEST_F(ConfigManagerTest, WindowGeometryIsSeparateFromTheTree) {
    SessionTree tree;
    ASSERT_TRUE(ConfigManager::saveTree(tree, path_));

    QSettings settings(QString::fromStdString(path_), QSettings::IniFormat);
    ConfigManager::saveWindowGeometry(settings, "geometry-blob");
    settings.sync();

    const SessionTree reloaded = ConfigManager::loadTree(path_);
    EXPECT_EQ(reloaded.size(), 0u) << "writing geometry must not disturb the tree";
}

TEST_F(ConfigManagerTest, SavingTheTreeLeavesTheWindowGeometryAlone) {
    // The reverse direction, and the one that used to be broken: saveTree() cleared the whole
    // file, so the geometry written a moment earlier was gone after the next save.
    QSettings settings(QString::fromStdString(path_), QSettings::IniFormat);
    ConfigManager::saveWindowGeometry(settings, "geometry-blob");
    settings.sync();

    SessionTree tree;
    ASSERT_TRUE(tree.add(profile("alpha")));
    ASSERT_TRUE(ConfigManager::saveTree(tree, path_));

    QSettings reloaded(QString::fromStdString(path_), QSettings::IniFormat);
    EXPECT_EQ(ConfigManager::loadWindowGeometry(reloaded), "geometry-blob")
        << "saving the tree must not wipe the window geometry";
    EXPECT_EQ(ConfigManager::loadTree(path_).size(), 1u);
}

}  // namespace
}  // namespace openscrt::config
