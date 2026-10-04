#include "widget/wversionlabel.h"

#include <QStringList>

#include "moc_wversionlabel.cpp"
#include "util/versionstore.h"

WVersionLabel::WVersionLabel(QWidget* pParent)
        : WLabel(pParent) {
}

void WVersionLabel::setup(const QDomNode& node, const SkinContext& context) {
    WLabel::setup(node, context);

    // Top line: what the unit is running. Bottom line: what it was built from,
    // i.e. the upstream Mixxx release plus the exact commit of this fork.
    QStringList productLine;
    productLine << VersionStore::productName() + QChar(' ') + VersionStore::version();
    const QString firmware = VersionStore::firmwareVersion();
    if (!firmware.isEmpty()) {
        productLine << tr("Firmware %1").arg(firmware);
    }

    const QStringList buildLine{
            QStringLiteral("Mixxx ") + VersionStore::mixxxVersion(),
            VersionStore::gitVersion()};

    const QString separator = QStringLiteral(" · ");
    setText(productLine.join(separator) + QChar('\n') + buildLine.join(separator));
}
