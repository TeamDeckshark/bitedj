// Visual tests for the Bite DJ Settings -> General page: the real
// settings.xml, styled by the real style.qss, built by the real
// LegacySkinParser, rendered offscreen at the panel's native 800x480 (the only
// resolution the device ships with). Every button on the page has to land fully on
// screen, at a touchable size, without overlapping another button or a row
// label, and the Wave Follows row (EQ / Trim waveform amplification) has to
// show both of its toggles side by side and drive the waveform factory.
//
// Each run also saves a screenshot per size, for a human to look at:
//
//   QT_QPA_PLATFORM=offscreen ./mixxx-test
//     --gtest_filter="*SettingsGeneralLayout*"
//
// writes settings_general_<w>x<h>.png into $BITEDJ_VISUAL_TEST_DIR, or into
// <tmp>/bitedj-visual-tests when that is unset.
//
// Only the General page is built. The other Settings pages hold widgets backed
// by singletons this harness does not start (controllers, audio devices,
// mounts), so they are dropped from the stack; the topbar and the deck strip
// under the page are stood in for by empty groups of the height they take in
// skin.xml / settings.xml, so the page gets the space it gets on the device.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <iostream>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "controllers/controllermanager.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "skin/legacy/legacyskinparser.h"
#include "test/mixxxtest.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/controlwidgetconnection.h"
#include "widget/wpushbutton.h"
#include "widget/wwidgetgroup.h"

namespace {

// The 52f rows of the General page carry 40f controls; anything smaller has
// been squeezed by the layout.
constexpr int kMinButtonSide = 40;

const QString kDeviceFont = QStringLiteral("DejaVu Sans");

const ConfigKey kApplyEqKey(QStringLiteral("[Waveform]"),
        QStringLiteral("apply_eq_to_waveform"));
const ConfigKey kApplyTrimKey(QStringLiteral("[Waveform]"),
        QStringLiteral("apply_trim_to_waveform"));

const QString kHarnessSkin = QStringLiteral(R"(<skin>
  <manifest>
    <title>Bite DJ settings harness</title>
    <attributes>
      <attribute config_key="[App],num_decks">4</attribute>
    </attributes>
  </manifest>
  <ObjectName>Mixxx</ObjectName>
  <Style src="skin:style.qss"/>
  <MinimumSize>480,420</MinimumSize>
  <SizePolicy>me,me</SizePolicy>
  <Layout>vertical</Layout>
  <Children>
    <WidgetGroup>
      <ObjectName>SkinContainer</ObjectName>
      <Layout>vertical</Layout>
      <SizePolicy>me,me</SizePolicy>
      <Children>
        <WidgetGroup>
          <Layout>vertical</Layout>
          <SizePolicy>me,me</SizePolicy>
          <Children>
            <WidgetGroup>
              <ObjectName>TopbarStack</ObjectName>
              <Size>0me,60f</Size>
            </WidgetGroup>
            <Template src="skin:settings_general_harness.xml"/>
          </Children>
        </WidgetGroup>
      </Children>
    </WidgetGroup>
  </Children>
</skin>
)");

QString skinSourceDir() {
    return QDir::cleanPath(
            MixxxTest::getOrInitTestDir().filePath(
                    QStringLiteral("../../res/skins/BiteDJ")));
}

bool copyTree(const QString& from, const QString& to) {
    QDirIterator it(from, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = to + QChar('/') + QDir(from).relativeFilePath(src);
        if (!QDir().mkpath(QFileInfo(dst).path()) || !QFile::copy(src, dst)) {
            return false;
        }
    }
    return true;
}

// settings.xml cut down to the General page, as described at the top.
bool writeGeneralOnlySettings(const QString& srcPath, const QString& dstPath) {
    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDomDocument doc;
    if (!doc.setContent(&src)) {
        return false;
    }

    const QDomNodeList stacks = doc.elementsByTagName(QStringLiteral("WidgetStack"));
    for (int i = 0; i < stacks.size(); ++i) {
        const QDomElement stack = stacks.at(i).toElement();
        if (stack.firstChildElement(QStringLiteral("ObjectName")).text() !=
                QStringLiteral("SettingsStack")) {
            continue;
        }
        QDomElement children = stack.firstChildElement(QStringLiteral("Children"));
        QDomElement page = children.firstChildElement();
        while (!page.isNull()) {
            const QDomElement next = page.nextSiblingElement();
            if (page.attribute(QStringLiteral("trigger")) !=
                    QStringLiteral("[SettingsTab],general")) {
                children.removeChild(page);
            }
            page = next;
        }
        // The real stack persists its page; a value left over from another
        // test must not pick a page that is no longer there.
        children.parentNode().toElement().removeAttribute(QStringLiteral("persist"));
    }

    const QDomNodeList groups = doc.elementsByTagName(QStringLiteral("WidgetGroup"));
    for (int i = 0; i < groups.size(); ++i) {
        QDomElement group = groups.at(i).toElement();
        if (group.firstChildElement(QStringLiteral("ObjectName")).text() !=
                QStringLiteral("Decks")) {
            continue;
        }
        // The deck strip is 0me,80max; stand in at its full 80 so the page is
        // tested with the least room it can get.
        group.removeChild(group.firstChildElement(QStringLiteral("Children")));
        group.firstChildElement(QStringLiteral("Size")).firstChild().setNodeValue(
                QStringLiteral("0me,80f"));
    }

    QFile dst(dstPath);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    QTextStream(&dst) << doc.toString(2);
    return true;
}

QString outputDir() {
    QString dir = qEnvironmentVariable("BITEDJ_VISUAL_TEST_DIR");
    if (dir.isEmpty()) {
        dir = QDir::temp().filePath(QStringLiteral("bitedj-visual-tests"));
    }
    QDir().mkpath(dir);
    return dir;
}

bool isConnectedTo(const WPushButton* pButton, const ConfigKey& key) {
    for (const auto* pConnection : pButton->connections()) {
        if (pConnection->getKey() == key) {
            return true;
        }
    }
    return false;
}

QString describe(const QWidget* pWidget, const QRect& rect) {
    QString name = pWidget->objectName();
    if (const auto* pLabel = qobject_cast<const QLabel*>(pWidget)) {
        name += QStringLiteral(" \"%1\"").arg(pLabel->text());
    } else if (const auto* pButton = qobject_cast<const WPushButton*>(pWidget)) {
        if (!pButton->connections().isEmpty()) {
            const ConfigKey& key = pButton->connections().first()->getKey();
            name += QStringLiteral(" %1,%2").arg(key.group, key.item);
        }
    }
    return QStringLiteral("%1 @ %2,%3 %4x%5")
            .arg(name)
            .arg(rect.x())
            .arg(rect.y())
            .arg(rect.width())
            .arg(rect.height());
}

class SettingsGeneralLayoutTest : public MixxxTest,
                                  public testing::WithParamInterface<QSize> {
  protected:
    void SetUp() override {
        // The skin asks for 'Inter', which the image does not carry, so on the
        // device it falls through to sans-serif, which fonts-deckshark.conf
        // pins to DejaVu Sans. A dev machine may well resolve both to
        // something else (Noto Sans, typically), with different widths, and
        // the widths are the whole point here.
        if (!QFontDatabase::families().contains(kDeviceFont)) {
            GTEST_SKIP() << kDeviceFont.toStdString()
                         << " is not installed; the layout would not be the device's";
        }
        QFont::insertSubstitution(QStringLiteral("Inter"), kDeviceFont);
        QFont::insertSubstitution(QStringLiteral("sans-serif"), kDeviceFont);

        // Every WWidget holds a proxy on this one.
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));

        // The factory owns the waveform COs, including the two Wave Follows
        // toggles; the skin would otherwise create stand-ins for them.
        WaveformWidgetFactory::createInstance();
        WaveformWidgetFactory::instance()->setConfig(config());

        ASSERT_TRUE(m_skinDir.isValid());
        ASSERT_TRUE(copyTree(skinSourceDir(), m_skinDir.path()))
                << "could not copy " << skinSourceDir().toStdString();
        ASSERT_TRUE(writeGeneralOnlySettings(
                skinSourceDir() + QStringLiteral("/settings.xml"),
                m_skinDir.filePath(QStringLiteral("settings_general_harness.xml"))));
        QFile skinXml(m_skinDir.filePath(QStringLiteral("skin.xml")));
        ASSERT_TRUE(skinXml.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QTextStream(&skinXml) << kHarnessSkin;
        skinXml.close();

        // The parser registers every widget with these two (MIDI learn and
        // shortcut tooltips). Neither touches the layout, and the controller
        // manager only looks for devices once setUpDevices() is called, which
        // nothing here does.
        m_pKbdConfig = std::make_unique<ConfigObject<ConfigValueKbd>>(QString());
        m_pKeyboard = std::make_unique<KeyboardEventFilter>(m_pKbdConfig.get());
        m_pControllerManager = std::make_shared<ControllerManager>(config());

        LegacySkinParser parser(config(),
                &m_skinCreatedControls,
                m_pKeyboard.get(),
                nullptr,
                m_pControllerManager.get(),
                nullptr,
                nullptr,
                nullptr,
                nullptr);
        m_pRoot.reset(parser.parseSkin(m_skinDir.path(), nullptr));
        ASSERT_NE(nullptr, m_pRoot);

        m_pRoot->resize(GetParam());
        m_pRoot->show();
        layOut();

        m_pPanel = findGeneralPanel();
        ASSERT_NE(nullptr, m_pPanel);
    }

    void TearDown() override {
        QFont::removeSubstitutions(QStringLiteral("Inter"));
        QFont::removeSubstitutions(QStringLiteral("sans-serif"));
        m_pRoot.reset();
        for (ControlObject* pControl : std::as_const(m_skinCreatedControls)) {
            delete pControl;
        }
        m_skinCreatedControls.clear();
        m_pControllerManager.reset();
        m_pKeyboard.reset();
        m_pKbdConfig.reset();
        WaveformWidgetFactory::destroy();
    }

    void layOut() {
        for (int i = 0; i < 3; ++i) {
            QCoreApplication::sendPostedEvents();
            if (m_pRoot->layout()) {
                m_pRoot->layout()->activate();
            }
            QCoreApplication::processEvents();
        }
    }

    QWidget* findGeneralPanel() const {
        const auto cols = m_pRoot->findChildren<QWidget*>(QStringLiteral("GeneralCol"));
        return cols.isEmpty() ? nullptr : cols.first()->parentWidget();
    }

    QRect rectInRoot(const QWidget* pWidget) const {
        return QRect(pWidget->mapTo(m_pRoot.get(), QPoint(0, 0)), pWidget->size());
    }

    QList<WPushButton*> visibleButtons() const {
        QList<WPushButton*> buttons;
        for (auto* pButton : m_pPanel->findChildren<WPushButton*>()) {
            if (pButton->isVisibleTo(m_pRoot.get())) {
                buttons.append(pButton);
            }
        }
        return buttons;
    }

    QList<QLabel*> visibleRowLabels() const {
        QList<QLabel*> labels;
        for (auto* pLabel : m_pPanel->findChildren<QLabel*>(
                     QStringLiteral("SettingsRowLabel"))) {
            if (pLabel->isVisibleTo(m_pRoot.get())) {
                labels.append(pLabel);
            }
        }
        return labels;
    }

    WPushButton* buttonFor(const ConfigKey& key) const {
        for (auto* pButton : visibleButtons()) {
            if (isConnectedTo(pButton, key)) {
                return pButton;
            }
        }
        return nullptr;
    }

    // Mimics WWidget::event()'s touch translation: a press then a release at
    // the centre of the button.
    void tap(WPushButton* pButton) {
        const QPointF pos = QRectF(pButton->rect()).center();
        const QPointF global = pButton->mapToGlobal(pos);
        QMouseEvent press(QEvent::MouseButtonPress,
                pos,
                pos,
                global,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(pButton, &press);
        QMouseEvent release(QEvent::MouseButtonRelease,
                pos,
                pos,
                global,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(pButton, &release);
        layOut();
    }

    void saveScreenshot(const QString& suffix = QString()) const {
        const QSize size = GetParam();
        const QString path = QDir(outputDir())
                                     .filePath(QStringLiteral("settings_general_%1x%2%3.png")
                                                     .arg(size.width())
                                                     .arg(size.height())
                                                     .arg(suffix));
        EXPECT_TRUE(m_pRoot->grab().save(path)) << path.toStdString();
        std::cout << "[ screenshot ] " << path.toStdString() << std::endl;
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<ConfigObject<ConfigValueKbd>> m_pKbdConfig;
    std::unique_ptr<KeyboardEventFilter> m_pKeyboard;
    std::shared_ptr<ControllerManager> m_pControllerManager;
    QTemporaryDir m_skinDir;
    QSet<ControlObject*> m_skinCreatedControls;
    std::unique_ptr<QWidget> m_pRoot;
    QWidget* m_pPanel = nullptr;
};

TEST_P(SettingsGeneralLayoutTest, RendersAtTheExpectedSize) {
    saveScreenshot();
    EXPECT_EQ(GetParam(), m_pRoot->size());
    EXPECT_FALSE(visibleButtons().isEmpty());
}

TEST_P(SettingsGeneralLayoutTest, EveryButtonIsFullyOnScreenAndTouchable) {
    const QRect page = rectInRoot(m_pPanel);
    for (auto* pButton : visibleButtons()) {
        const QRect rect = rectInRoot(pButton);
        const std::string what = describe(pButton, rect).toStdString();
        EXPECT_TRUE(m_pRoot->rect().contains(rect)) << what << " leaves the window";
        EXPECT_TRUE(page.contains(rect)) << what << " leaves the General page";
        // Clipped by an ancestor or painted over by a sibling.
        EXPECT_EQ(pButton->rect(), pButton->visibleRegion().boundingRect())
                << what << " is not fully visible";
        EXPECT_GE(rect.width(), kMinButtonSide) << what << " is too narrow";
        EXPECT_GE(rect.height(), kMinButtonSide) << what << " is too short";
    }
}

TEST_P(SettingsGeneralLayoutTest, NoTwoButtonsOverlap) {
    const auto buttons = visibleButtons();
    for (int i = 0; i < buttons.size(); ++i) {
        const QRect a = rectInRoot(buttons[i]);
        for (int j = i + 1; j < buttons.size(); ++j) {
            const QRect b = rectInRoot(buttons[j]);
            EXPECT_FALSE(a.intersects(b))
                    << describe(buttons[i], a).toStdString() << " overlaps "
                    << describe(buttons[j], b).toStdString();
        }
    }
}

TEST_P(SettingsGeneralLayoutTest, NoButtonCoversARowLabel) {
    const auto buttons = visibleButtons();
    for (auto* pLabel : visibleRowLabels()) {
        const QRect labelRect = rectInRoot(pLabel);
        for (auto* pButton : buttons) {
            const QRect buttonRect = rectInRoot(pButton);
            EXPECT_FALSE(labelRect.intersects(buttonRect))
                    << describe(pButton, buttonRect).toStdString() << " covers "
                    << describe(pLabel, labelRect).toStdString();
        }
    }
}

TEST_P(SettingsGeneralLayoutTest, RowLabelsAreNotCutOff) {
    for (auto* pLabel : visibleRowLabels()) {
        const QRect rect = rectInRoot(pLabel);
        const int needed = pLabel->fontMetrics().horizontalAdvance(pLabel->text());
        EXPECT_LE(needed, pLabel->contentsRect().width())
                << describe(pLabel, rect).toStdString() << " needs " << needed << "px";
        EXPECT_EQ(pLabel->rect(), pLabel->visibleRegion().boundingRect())
                << describe(pLabel, rect).toStdString() << " is not fully visible";
    }
}

TEST_P(SettingsGeneralLayoutTest, WaveFollowsRowShowsEqThenTrim) {
    WPushButton* pEq = buttonFor(kApplyEqKey);
    WPushButton* pTrim = buttonFor(kApplyTrimKey);
    ASSERT_NE(nullptr, pEq);
    ASSERT_NE(nullptr, pTrim);

    // Both sit in the one row, under the row's own label.
    QWidget* pRow = pEq->parentWidget()->parentWidget();
    ASSERT_EQ(pRow, pTrim->parentWidget()->parentWidget());
    EXPECT_EQ(QStringLiteral("SettingsRow"), pRow->objectName());
    const auto labels = pRow->findChildren<QLabel*>(QStringLiteral("SettingsRowLabel"));
    ASSERT_EQ(1, labels.size());
    EXPECT_EQ(QStringLiteral("Wave Follows"), labels.first()->text());

    const QRect row = rectInRoot(pRow);
    const QRect label = rectInRoot(labels.first());
    const QRect eq = rectInRoot(pEq);
    const QRect trim = rectInRoot(pTrim);
    EXPECT_TRUE(row.contains(eq));
    EXPECT_TRUE(row.contains(trim));
    EXPECT_LE(label.right(), eq.left()) << "label runs into EQ";
    EXPECT_LE(eq.right(), trim.left()) << "EQ is not left of Trim";
    EXPECT_EQ(eq.top(), trim.top());
    EXPECT_EQ(eq.height(), trim.height());
    EXPECT_EQ(eq.width(), trim.width());
}

TEST_P(SettingsGeneralLayoutTest, WaveFollowsTogglesAreIndependentAndPersist) {
    WaveformWidgetFactory* pFactory = WaveformWidgetFactory::instance();
    WPushButton* pEq = buttonFor(kApplyEqKey);
    WPushButton* pTrim = buttonFor(kApplyTrimKey);
    ASSERT_NE(nullptr, pEq);
    ASSERT_NE(nullptr, pTrim);

    // Stock behaviour on a fresh config: the wave follows both.
    EXPECT_TRUE(pFactory->isEqAppliedToWaveform());
    EXPECT_TRUE(pFactory->isTrimAppliedToWaveform());
    EXPECT_EQ(1, pEq->readDisplayValue());
    EXPECT_EQ(1, pTrim->readDisplayValue());

    tap(pTrim);
    saveScreenshot(QStringLiteral("_trim_off"));
    EXPECT_FALSE(pFactory->isTrimAppliedToWaveform());
    EXPECT_TRUE(pFactory->isEqAppliedToWaveform());
    EXPECT_EQ(0, pTrim->readDisplayValue());
    EXPECT_EQ(1, pEq->readDisplayValue());
    EXPECT_EQ(0.0, ControlObject::get(kApplyTrimKey));
    EXPECT_FALSE(config()->getValue(ConfigKey("[Waveform]", "ApplyTrimToWaveform"), true));

    tap(pEq);
    EXPECT_FALSE(pFactory->isEqAppliedToWaveform());
    EXPECT_FALSE(pFactory->isTrimAppliedToWaveform());
    EXPECT_EQ(0, pEq->readDisplayValue());

    tap(pTrim);
    EXPECT_TRUE(pFactory->isTrimAppliedToWaveform());
    EXPECT_FALSE(pFactory->isEqAppliedToWaveform());
    EXPECT_EQ(1, pTrim->readDisplayValue());
    EXPECT_TRUE(config()->getValue(ConfigKey("[Waveform]", "ApplyTrimToWaveform"), false));
}

INSTANTIATE_TEST_SUITE_P(BiteDJ,
        SettingsGeneralLayoutTest,
        testing::Values(QSize(800, 480)),
        [](const testing::TestParamInfo<QSize>& info) {
            return QStringLiteral("%1x%2")
                    .arg(info.param.width())
                    .arg(info.param.height())
                    .toStdString();
        });

} // namespace
