#include <QFile>
#include <QString>

#include "control/control.h"
#include "test/mixxxtest.h"
#include "preferences/configobject.h"

namespace {

class ConfigObjectTest : public MixxxTest {
  protected:
    // Writes to the config file behind the live UserSettings' back, the way
    // anything outside Mixxx (image provisioning, a hand edit) would.
    void appendToConfigFile(const QString& text) {
        QFile file(getTestDataDir().filePath("test.cfg"));
        ASSERT_TRUE(file.open(QIODevice::Append | QIODevice::Text));
        ASSERT_NE(-1, file.write(text.toUtf8()));
        file.close();
    }

    // Re-reads the config file without saving first, i.e. picks up an external
    // edit instead of overwriting it.
    void reloadConfig() {
        m_pConfig = UserSettingsPointer(
                new UserSettings(getTestDataDir().filePath("test.cfg")));
        ControlDoublePrivate::setUserConfig(m_pConfig);
    }
};

TEST_F(ConfigObjectTest, SetValue_Overwrite) {
    auto ck = ConfigKey("[Test]", "test");
    config()->setValue(ck, QString("asdf"));
    config()->setValue(ck, QString("zxcv"));
    EXPECT_QSTRING_EQ("zxcv", config()->getValue<QString>(ck));
}

TEST_F(ConfigObjectTest, SetValue_QString) {
    auto ck = ConfigKey("[Test]", "test");
    config()->setValue(ck, QString("asdf"));
    EXPECT_QSTRING_EQ("asdf", config()->getValue<QString>(ck));
}

TEST_F(ConfigObjectTest, SetValue_QString_NullVsEmpty) {
    auto ck = ConfigKey("[Test]", "test");
    EXPECT_TRUE(config()->getValue<QString>(ck).isNull());

    // Setting it to an empty string returns an empty, not null string. Empty
    // strings are counted as present by the default value logic.
    config()->setValue(ck, QString(""));
    EXPECT_TRUE(config()->getValue<QString>(ck, QString("asdf")).isEmpty());
    EXPECT_FALSE(config()->getValue<QString>(ck, QString("asdf")).isNull());

    // And it persists across restarts.
    saveAndReloadConfig();
    EXPECT_TRUE(config()->getValue<QString>(ck).isEmpty());
    EXPECT_FALSE(config()->getValue<QString>(ck).isNull());
}

TEST_F(ConfigObjectTest, GetValue_QString) {
    auto ck = ConfigKey("[Test]", "test");
    EXPECT_QSTRING_EQ("zxcv", config()->getValue<QString>(ck, "zxcv"));
    config()->setValue(ck, QString("asdf"));
    EXPECT_QSTRING_EQ("asdf", config()->getValue<QString>(ck, "zxcv"));
}

TEST_F(ConfigObjectTest, SetValue_Integer) {
    auto ck = ConfigKey("[Test]", "test");
    config()->setValue(ck, 5);
    EXPECT_QSTRING_EQ("5", config()->getValue<QString>(ck));
}

TEST_F(ConfigObjectTest, GetValue_Integer) {
    auto ck = ConfigKey("[Test]", "test");

    // Not present.
    EXPECT_EQ(5, config()->getValue(ck, 5));

    // Empty
    config()->setValue(ck, QString(""));
    EXPECT_EQ(5, config()->getValue(ck, 5));

    // Malformatted.
    config()->setValue(ck, QString("asdf"));
    EXPECT_EQ(5, config()->getValue(ck, 5));

    // Overflow 32-bit int.
    config()->setValue(ck, QString("2147483648"));
    EXPECT_EQ(5, config()->getValue(ck, 5));
    config()->setValue(ck, QString("-2147483649"));
    EXPECT_EQ(5, config()->getValue(ck, 5));

    // Ok.
    config()->setValue(ck, QString("4"));
    EXPECT_EQ(4, config()->getValue(ck, 5));
    config()->setValue(ck, QString("-4"));
    EXPECT_EQ(-4, config()->getValue(ck, 5));
}

TEST_F(ConfigObjectTest, SetValue_Bool) {
    auto ck = ConfigKey("[Test]", "test");
    config()->setValue(ck, true);
    EXPECT_QSTRING_EQ("1", config()->getValue<QString>(ck));
    config()->setValue(ck, false);
    EXPECT_QSTRING_EQ("0", config()->getValue<QString>(ck));
}

TEST_F(ConfigObjectTest, GetValue_Bool) {
    auto ck = ConfigKey("[Test]", "test");

    // Not present.
    EXPECT_TRUE(config()->getValue(ck, true));

    // Empty
    config()->setValue(ck, QString(""));
    EXPECT_TRUE(config()->getValue(ck, true));

    // Malformatted.
    config()->setValue(ck, QString("asdf"));
    EXPECT_TRUE(config()->getValue(ck, true));

    // Ok.
    config()->setValue(ck, QString("0"));
    EXPECT_FALSE(config()->getValue(ck, true));

    // Not just 0 and 1.
    config()->setValue(ck, QString("5"));
    EXPECT_TRUE(config()->getValue(ck, false));
}

TEST_F(ConfigObjectTest, Exists) {
    auto ck = ConfigKey("[Test]", "test");
    EXPECT_FALSE(config()->exists(ck));
    config()->setValue(ck, 5);
    EXPECT_TRUE(config()->exists(ck));
}

TEST_F(ConfigObjectTest, GetValueString) {
    auto ck = ConfigKey("[Test]", "foo");
    auto ck2 = ConfigKey("[Test]", "bar");
    config()->setValue(ck, 5);
    EXPECT_QSTRING_EQ("5", config()->getValueString(ck));
    EXPECT_QSTRING_EQ("6", config()->getValue(ck2, "6"));
}

TEST_F(ConfigObjectTest, Save) {
    for (int i = 0; i < 10; ++i) {
        config()->setValue(ConfigKey(QString("[Test%1]").arg(i),
                                     QString("control%1").arg(i)), i);
    }

    m_pConfig->save();
    m_pConfig = UserSettingsPointer(
            new UserSettings(getTestDataDir().filePath("test.cfg")));

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(i, config()->getValue<int>(
            ConfigKey(QString("[Test%1]").arg(i),
                      QString("control%1").arg(i)), -1));
    }
}

// [BiteDJ],usb_drive_path_<N> is hardware provisioning owned by the config
// file, not a preference Mixxx may write. The three tests below pin the three
// ways it could be lost: overwritten, removed, or dropped by the full-file
// rewrite in save().

TEST_F(ConfigObjectTest, UsbDrivePath_SetIsRefused) {
    const auto ck = ConfigKey("[BiteDJ]", "usb_drive_path_1");
    config()->setValue(ck, QString("1-1.5"));
    EXPECT_FALSE(config()->exists(ck));

    // Not even over a value that came from the file.
    appendToConfigFile("[BiteDJ]\nusb_drive_path_1 1-1.5\n");
    reloadConfig();
    config()->setValue(ck, QString("2-1.1"));
    EXPECT_QSTRING_EQ("1-1.5", config()->getValue<QString>(ck));
}

TEST_F(ConfigObjectTest, UsbDrivePath_RemoveIsRefused) {
    const auto ck = ConfigKey("[BiteDJ]", "usb_drive_path_2");
    appendToConfigFile("[BiteDJ]\nusb_drive_path_2 1-1.4\n");
    reloadConfig();

    EXPECT_FALSE(config()->remove(ck));
    EXPECT_QSTRING_EQ("1-1.4", config()->getValue<QString>(ck));

    saveAndReloadConfig();
    EXPECT_QSTRING_EQ("1-1.4", config()->getValue<QString>(ck));
}

TEST_F(ConfigObjectTest, UsbDrivePath_SavePreservesEntriesAddedToFileAtRuntime) {
    // Mixxx has the file parsed and running; the entries land in it afterwards
    // (image provisioning, a hand edit) and Mixxx never re-parses. A plain
    // rewrite from the in-memory map would erase them.
    appendToConfigFile("[BiteDJ]\nusb_drive_path_1 1-1.5\nusb_drive_path_2 1-1.4\n");
    config()->setValue(ConfigKey("[Test]", "test"), QString("asdf"));

    saveAndReloadConfig();
    EXPECT_QSTRING_EQ("1-1.5",
            config()->getValue<QString>(ConfigKey("[BiteDJ]", "usb_drive_path_1")));
    EXPECT_QSTRING_EQ("1-1.4",
            config()->getValue<QString>(ConfigKey("[BiteDJ]", "usb_drive_path_2")));
    // Neighbouring [BiteDJ] keys stay writable.
    config()->setValue(ConfigKey("[BiteDJ]", "vinyl_mode"), 1);
    saveAndReloadConfig();
    EXPECT_EQ(1, config()->getValue<int>(ConfigKey("[BiteDJ]", "vinyl_mode"), -1));
    EXPECT_QSTRING_EQ("1-1.5",
            config()->getValue<QString>(ConfigKey("[BiteDJ]", "usb_drive_path_1")));
}

// [Logging],Path and [Logging],KeepFiles are file-authoritative for the same
// reason: hand-edited into mixxx.cfg, read once at startup, never written by
// Mixxx. Editing them while Mixxx runs is the normal way to change them, so
// the autosave must not undo the edit before the restart applies it.
TEST_F(ConfigObjectTest, LoggingSettings_AreFileAuthoritative) {
    const auto pathKey = ConfigKey("[Logging]", "Path");
    const auto keepKey = ConfigKey("[Logging]", "KeepFiles");
    appendToConfigFile("[Logging]\nPath /var/log\nKeepFiles 3\n");
    reloadConfig();

    config()->setValue(pathKey, QString("/tmp/elsewhere"));
    config()->setValue(keepKey, 10);
    EXPECT_FALSE(config()->remove(pathKey));
    EXPECT_QSTRING_EQ("/var/log", config()->getValue<QString>(pathKey));
    EXPECT_EQ(3, config()->getValue<int>(keepKey, -1));

    // An edit made to the file while Mixxx is running wins over both the
    // in-memory value and the rewrite that save() performs.
    appendToConfigFile("[Logging]\nPath /srv/logs\nKeepFiles 5\n");
    saveAndReloadConfig();
    EXPECT_QSTRING_EQ("/srv/logs", config()->getValue<QString>(pathKey));
    EXPECT_EQ(5, config()->getValue<int>(keepKey, -1));

    // Unrelated keys in the group stay writable.
    config()->setValue(ConfigKey("[Logging]", "Other"), QString("x"));
    saveAndReloadConfig();
    EXPECT_QSTRING_EQ("x", config()->getValue<QString>(ConfigKey("[Logging]", "Other")));
}

// save() publishes the new file by renaming the temp file over the old one in
// a single step. It used to unlink the old one first, which left a window with
// no config file at all — fatal if the appliance is killed or loses power
// there, because usb_drive_path_<N> is the one thing nothing regenerates.
TEST_F(ConfigObjectTest, Save_ReplacesExistingFileAtomically) {
    const QString path = getTestDataDir().filePath("test.cfg");
    const auto ck = ConfigKey("[Test]", "test");

    config()->setValue(ck, QString("first"));
    ASSERT_TRUE(m_pConfig->save());
    ASSERT_TRUE(QFile::exists(path));

    // Saving again has to overwrite the now-existing destination, not fail on
    // it (QFile::rename() refuses to, which is why the unlink was there).
    config()->setValue(ck, QString("second"));
    EXPECT_TRUE(m_pConfig->save());
    EXPECT_TRUE(QFile::exists(path));
    // ... and the temp file must not be left behind next to it.
    EXPECT_FALSE(QFile::exists(path + ".tmp"));

    reloadConfig();
    EXPECT_QSTRING_EQ("second", config()->getValue<QString>(ck));
}

}  // namespace
