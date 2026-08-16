#include "widget/wsamplerdrive.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QStyle>

#include "mixer/samplerdrive.h"
#include "moc_wsamplerdrive.cpp"
#include "skin/legacy/skincontext.h"

namespace {
const char* kChipObjectName = "SamplerDriveChip";
const char* kPlaceholderObjectName = "SamplerDrivePlaceholder";
const char* kSelectedProperty = "selected";

// Repolishes a widget so a property change picked up by the stylesheet
// ([selected]) is actually repainted.
void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}
} // namespace

WSamplerDrive::WSamplerDrive(QWidget* parent)
        : WWidget(parent),
          m_pLayout(new QHBoxLayout(this)),
          m_pPlaceholder(nullptr),
          m_selectedIndex(-1),
          m_pressed(false) {
    setAttribute(Qt::WA_StyledBackground, true);
    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setSpacing(6);

    SamplerDrive* pDrive = SamplerDrive::tryInstance();
    if (pDrive) {
        connect(pDrive,
                &SamplerDrive::drivesChanged,
                this,
                &WSamplerDrive::onDrivesChanged);
        rebuild(pDrive->driveLabels(), pDrive->selectedDriveIndex());
    } else {
        renderEmpty();
    }
}

WSamplerDrive::~WSamplerDrive() = default;

void WSamplerDrive::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by SamplerDrive.
}

void WSamplerDrive::onDrivesChanged(const QStringList& labels, int selectedIndex) {
    rebuild(labels, selectedIndex);
}

void WSamplerDrive::clearChildren() {
    // Detach immediately (so they stop painting) but deleteLater, so Qt can
    // finish dispatching the event that triggered the rebuild — a synchronous
    // delete from inside a chip's own clicked handler crashes when the queued
    // mouse release fires on the freed widget.
    for (QPushButton* pChip : std::as_const(m_chips)) {
        pChip->setParent(nullptr);
        pChip->deleteLater();
    }
    m_chips.clear();
    if (m_pPlaceholder) {
        m_pPlaceholder->setParent(nullptr);
        m_pPlaceholder->deleteLater();
        m_pPlaceholder = nullptr;
    }
}

void WSamplerDrive::rebuild(const QStringList& labels, int selectedIndex) {
    clearChildren();
    m_labels = labels;
    m_selectedIndex = selectedIndex;

    if (labels.isEmpty()) {
        renderEmpty();
        return;
    }
    for (int i = 0; i < labels.size(); ++i) {
        auto* pChip = new QPushButton(labels.at(i), this);
        pChip->setObjectName(kChipObjectName);
        pChip->setFocusPolicy(Qt::NoFocus);
        // Ignored, so several long volume names share the strip evenly instead
        // of the widest one deciding how much is left for the others.
        pChip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        pChip->setProperty(kSelectedProperty, i == selectedIndex);
        connect(pChip, &QPushButton::clicked, this, &WSamplerDrive::onChipClicked);
        m_pLayout->addWidget(pChip);
        m_chips.append(pChip);
        restyle(style(), pChip);
    }
    applyElidedLabels();
}

void WSamplerDrive::renderEmpty() {
    SamplerDrive* pDrive = SamplerDrive::tryInstance();
    const QString selectedName = pDrive ? pDrive->selectedDriveName() : QString();
    m_pPlaceholder = new QLabel(selectedName.isEmpty()
                    ? tr("No USB drives")
                    : tr("%1 not connected").arg(selectedName),
            this);
    m_pPlaceholder->setObjectName(kPlaceholderObjectName);
    m_pPlaceholder->setAlignment(Qt::AlignCenter);
    m_pLayout->addWidget(m_pPlaceholder);
    restyle(style(), m_pPlaceholder);
}

void WSamplerDrive::resizeEvent(QResizeEvent* e) {
    WWidget::resizeEvent(e);
    applyElidedLabels();
}

void WSamplerDrive::applyElidedLabels() {
    for (int i = 0; i < m_chips.size(); ++i) {
        QPushButton* pChip = m_chips.at(i);
        // Leave room for the chip's own padding; too tight simply elides a
        // character early, too loose would let the text touch the edge.
        constexpr int kTextInsetPx = 24;
        const int available = pChip->width() - kTextInsetPx;
        if (available <= 0) {
            continue;
        }
        pChip->setText(pChip->fontMetrics().elidedText(
                m_labels.at(i), Qt::ElideRight, available));
    }
}

void WSamplerDrive::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }
    m_pressed = true;
    e->accept();
}

void WSamplerDrive::mouseReleaseEvent(QMouseEvent* e) {
    const bool pressed = m_pressed;
    m_pressed = false;
    if (pressed) {
        const int index = chipIndexAt(e->globalPosition().toPoint());
        if (index >= 0) {
            if (SamplerDrive* pDrive = SamplerDrive::tryInstance()) {
                // Selecting re-emits drivesChanged, which rebuilds the chips —
                // including this one — so nothing is painted here.
                pDrive->selectDriveAt(index);
            }
            e->accept();
            return;
        }
    }
    WWidget::mouseReleaseEvent(e);
}

void WSamplerDrive::onChipClicked() {
    // Desktop/mouse path: a real QMouseEvent reaches the child button directly
    // and emits clicked(). (On the touchscreen this never fires; the release
    // handler above does the dispatching instead.)
    const int index = m_chips.indexOf(qobject_cast<QPushButton*>(sender()));
    if (index < 0) {
        return;
    }
    if (SamplerDrive* pDrive = SamplerDrive::tryInstance()) {
        pDrive->selectDriveAt(index);
    }
}

int WSamplerDrive::chipIndexAt(const QPoint& globalPos) const {
    for (int i = 0; i < m_chips.size(); ++i) {
        const QPushButton* pChip = m_chips.at(i);
        if (pChip->rect().contains(pChip->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}
