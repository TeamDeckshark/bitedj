#include "widget/wversionlabel.h"

#include <QFile>

#include "moc_wversionlabel.cpp"
#include "util/versionstore.h"

namespace {

const QString kReleaseFilePath = QStringLiteral("/etc/os-release");

QString firmwareVersion() {
    QFile file(kReleaseFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        constexpr auto kVersionKey = QLatin1String("VERSION=");
        if (line.startsWith(kVersionKey)) {
            return line.mid(kVersionKey.size()).trimmed();
        }
    }
    return QString();
}

} // anonymous namespace

WVersionLabel::WVersionLabel(QWidget* pParent)
        : WLabel(pParent) {
}

void WVersionLabel::setup(const QDomNode& node, const SkinContext& context) {
    WLabel::setup(node, context);

    const QVersionNumber versionNumber = VersionStore::versionNumber();
    const QString buildLine = QStringLiteral("%1.%2 · %3")
                                      .arg(QString::number(versionNumber.majorVersion()),
                                              QString::number(versionNumber.minorVersion()),
                                              VersionStore::gitVersion());

    const QString firmware = firmwareVersion();
    if (firmware.isEmpty()) {
        setText(buildLine);
    } else {
        setText(QStringLiteral("%1\nFirmware %2").arg(buildLine, firmware));
    }
}
