#pragma once

#include <QDateTime>
#include <QString>
#include <QVersionNumber>

/// Bite DJ carries two versions:
///
/// * The *product* version -- version(), versionNumber(), versionSuffix() --
///   is the Bite DJ fork version (e.g. 1.0). This is what the UI displays and
///   what `--version` reports.
/// * The *base* version -- mixxxVersion() and friends -- is the upstream Mixxx
///   stable release this fork was built on (e.g. 2.5.6). It is not displayed as
///   the product version, but it is preserved so the base can be tracked, and
///   it still keys the config-file upgrade path (see preferences/upgrade.cpp)
///   and the Mixxx manual URLs (see defs_urls.h).
///
/// Both are set in the top-level CMakeLists.txt: BITEDJ_VERSION for the
/// product, project(mixxx VERSION ...) for the base.
class VersionStore {
  public:
    /// Returns the Bite DJ version string (e.g. 1.0, or 1.0-beta)
    static QString version();

    /// Returns the Bite DJ version number (e.g. 1.0)
    static QVersionNumber versionNumber();

    /// Returns the Bite DJ version suffix (e.g. "beta")
    static QString versionSuffix();

    /// Returns the Mixxx version string this fork is based on (e.g. 2.5.6)
    static QString mixxxVersion();

    /// Returns the Mixxx version number this fork is based on (e.g. 2.5.6)
    static QVersionNumber mixxxVersionNumber();

    /// Returns the Mixxx version suffix this fork is based on (e.g. "beta")
    static QString mixxxVersionSuffix();

    /// Returns the product name shown to the user. (e.g. "Bite DJ")
    static QString productName();

    /// Returns the application name used for the window title, the JACK client
    /// name and native dialog titles. (e.g. "Mixxx")
    static QString applicationName();

    /// Returns the last change date
    static QDateTime date();

    /// Returns the platform (e.g. "Windows x86_64")
    static QString platform();

    /// Returns the git branch (e.g. features_key) or the null
    /// string if the branch is unknown.
    static QString gitBranch();

    /// Returns the output of "git describe"
    static QString gitDescribe();

    /// Returns the output of "git describe" and the branch name (if available)
    static QString gitVersion();

    /// Returns the version of Qt used to build Mixxx.
    static QString qtVersion();

    /// Returns the build flags used to build Mixxx (e.g. "hid=1 modplug=0") or
    /// the null string if the flags are unknown.
    static QString buildFlags();

    /// Returns a list of the version of each dependency:
    static QStringList dependencyVersions();

    /// Prints out diagnostic information about this build.
    static void logBuildDetails();
};
