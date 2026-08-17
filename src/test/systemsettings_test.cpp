#include "preferences/systemsettings.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QStringList>

namespace {

class ScopedUserEnvironment {
  public:
    explicit ScopedUserEnvironment(const QByteArray& user)
            : m_wasSet(qEnvironmentVariableIsSet("USER")),
              m_previous(qgetenv("USER")) {
        qputenv("USER", user);
    }

    ~ScopedUserEnvironment() {
        if (m_wasSet) {
            qputenv("USER", m_previous);
        } else {
            qunsetenv("USER");
        }
    }

  private:
    bool m_wasSet;
    QByteArray m_previous;
};

TEST(SystemSettingsTest, RemovableRootsIncludeCurrentUserMountDirectories) {
    const ScopedUserEnvironment user("bitedj-test-user");

    const QStringList roots = SystemSettings::removableRoots();

    EXPECT_TRUE(roots.contains(QStringLiteral("/media/bitedj-test-user")));
    EXPECT_TRUE(roots.contains(QStringLiteral("/run/media/bitedj-test-user")));
}

TEST(SystemSettingsTest, RemovableRootsDoNotDuplicateBasePathsWithoutUser) {
    const ScopedUserEnvironment user("");

    const QStringList roots = SystemSettings::removableRoots();

    EXPECT_EQ(1, roots.count(QStringLiteral("/media")));
    EXPECT_EQ(1, roots.count(QStringLiteral("/run/media")));
    EXPECT_FALSE(roots.contains(QStringLiteral("/media/")));
    EXPECT_FALSE(roots.contains(QStringLiteral("/run/media/")));
}

} // namespace
