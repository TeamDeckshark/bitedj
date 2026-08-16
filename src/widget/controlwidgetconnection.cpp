#include "widget/controlwidgetconnection.h"

#include <QStyle>

#include "control/controlproxy.h"
#include "moc_controlwidgetconnection.cpp"
#include "util/assert.h"
#include "util/valuetransformer.h"
#include "widget/wbasewidget.h"

namespace {

QMetaProperty propertyFromWidget(const QWidget* pWidget, const QString& name) {
    VERIFY_OR_DEBUG_ASSERT(!name.isEmpty()) {
        return QMetaProperty();
    }
    VERIFY_OR_DEBUG_ASSERT(pWidget) {
        return QMetaProperty();
    }
    const QMetaObject* meta = pWidget->metaObject();
    VERIFY_OR_DEBUG_ASSERT(meta) {
        return QMetaProperty();
    }
    const int id = meta->indexOfProperty(name.toLatin1().constData());
    // Bite DJ fork: a missing Q_PROPERTY is not an error. `BindProperty`
    // falls back to QWidget::setProperty (dynamic property) so skins can drive
    // QSS attribute selectors (e.g. `#Sampler[loaded="1"]`) without forcing
    // every targeted widget class to declare a matching Q_PROPERTY.
    if (id < 0) {
        return QMetaProperty();
    }
    return meta->property(id);
}

} // namespace

ControlWidgetConnection::ControlWidgetConnection(
        WBaseWidget* pBaseWidget,
        const ConfigKey& key,
        ValueTransformer* pTransformer)
        : m_pWidget(pBaseWidget),
          m_pValueTransformer(pTransformer) {
    m_pControl = new ControlProxy(key, this, ControlFlag::NoAssertIfMissing);
    m_pControl->connectValueChanged(this, &ControlWidgetConnection::slotControlValueChanged);
}

void ControlWidgetConnection::setControlParameter(double parameter) {
    if (m_pValueTransformer != nullptr) {
        parameter = m_pValueTransformer->transformInverse(parameter);
    }
    m_pControl->setParameter(parameter);
}

double ControlWidgetConnection::getControlParameter() const {
    double parameter = m_pControl->getParameter();
    if (m_pValueTransformer != nullptr) {
        parameter = m_pValueTransformer->transform(parameter);
    }
    return parameter;
}

double ControlWidgetConnection::getControlParameterForValue(double value) const {
    double parameter = m_pControl->getParameterForValue(value);
    if (m_pValueTransformer != nullptr) {
        parameter = m_pValueTransformer->transform(parameter);
    }
    return parameter;
}

ControlParameterWidgetConnection::ControlParameterWidgetConnection(
        WBaseWidget* pBaseWidget, const ConfigKey& key,
        ValueTransformer* pTransformer, DirectionOption directionOption,
        EmitOption emitOption)
        : ControlWidgetConnection(pBaseWidget, key, pTransformer),
          m_directionOption(directionOption),
          m_emitOption(emitOption) {
}

void ControlParameterWidgetConnection::Init() {
    slotControlValueChanged(m_pControl->get());
}

QString ControlParameterWidgetConnection::toDebugString() const {
    const ConfigKey& key = getKey();
    return QString("%1,%2 Parameter: %3 Value: %4 Direction: %5 Emit: %6")
            .arg(key.group, key.item,
                 QString::number(m_pControl->getParameter()),
                 QString::number(m_pControl->get()),
                 directionOptionToString(m_directionOption),
                 emitOptionToString(m_emitOption));
}

void ControlParameterWidgetConnection::slotControlValueChanged(double value) {
    if (m_directionOption & DIR_TO_WIDGET) {
        const double parameter = getControlParameterForValue(value);
        m_pWidget->onConnectedControlChanged(parameter, value);
    }
}

void ControlParameterWidgetConnection::resetControl() {
    if (m_directionOption & DIR_FROM_WIDGET) {
        m_pControl->reset();
    }
}

void ControlParameterWidgetConnection::setControlParameter(double v) {
    if (m_directionOption & DIR_FROM_WIDGET) {
        ControlWidgetConnection::setControlParameter(v);
    }
}

void ControlParameterWidgetConnection::setControlParameterDown(double v) {
    if ((m_directionOption & DIR_FROM_WIDGET) && (m_emitOption & EMIT_ON_PRESS)) {
        ControlWidgetConnection::setControlParameter(v);
    }
}

void ControlParameterWidgetConnection::setControlParameterUp(double v) {
    if ((m_directionOption & DIR_FROM_WIDGET) && (m_emitOption & EMIT_ON_RELEASE)) {
        ControlWidgetConnection::setControlParameter(v);
    }
}

ControlWidgetPropertyConnection::ControlWidgetPropertyConnection(
        WBaseWidget* pBaseWidget,
        const ConfigKey& key,
        ValueTransformer* pTransformer,
        const QString& propertyName)
        : ControlWidgetConnection(pBaseWidget, key, pTransformer),
          m_propertyName(propertyName),
          m_property(propertyFromWidget(pBaseWidget->toQWidget(), propertyName)) {
    // Initial update to synchronize the property in all the sub widgets
    slotControlValueChanged(m_pControl->get());
}

QString ControlWidgetPropertyConnection::toDebugString() const {
    const ConfigKey& key = getKey();
    const QWidget* pWidget = m_pWidget->toQWidget();
    const QString value = m_property.isValid()
            ? m_property.read(pWidget).toString()
            : pWidget->property(m_propertyName.toLatin1().constData()).toString();
    return QString("%1,%2 Parameter: %3 Property: %4 Value: %5")
            .arg(key.group,
                    key.item,
                    QString::number(m_pControl->getParameter()),
                    m_propertyName,
                    value);
}

void ControlWidgetPropertyConnection::slotControlValueChanged(double v) {
    const double parameter = getControlParameterForValue(v);
    QWidget* pWidget = m_pWidget->toQWidget();

    // Bite DJ fork: dynamic-property path when no Q_PROPERTY of this name is
    // declared on the widget. Writes as int so QSS attribute selectors like
    // `[loaded="1"]` match. Avoids the QMetaProperty assert cascade and keeps
    // skin styling on widgets that don't (and shouldn't have to) declare every
    // bindable flag as Q_PROPERTY.
    if (!m_property.isValid()) {
        const QVariant vParameter(static_cast<int>(parameter));
        if (m_propertyValue == vParameter) {
            return;
        }
        m_propertyValue = vParameter;
        pWidget->setProperty(m_propertyName.toLatin1().constData(), vParameter);
        pWidget->style()->unpolish(pWidget);
        pWidget->style()->polish(pWidget);
        pWidget->update();
        return;
    }

    QVariant vParameter;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_property.metaType().id() == QMetaType::Bool) {
#else
    if (m_property.type() == QVariant::Bool) {
#endif
        vParameter = (parameter > 0);
    } else {
        vParameter = parameter;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool success = vParameter.convert(m_property.metaType());
#else
    bool success = vParameter.convert(m_property.type());
#endif
    VERIFY_OR_DEBUG_ASSERT(success) {
        return;
    }
    if (m_propertyValue == vParameter) {
        // don't repeat writing the same value that may stall the GUI
        // Comparing the property value directly does not work, because
        // in some cases (e.g. visible) it has to be propagated to the child widgets.
        return;
    }

    if (!m_property.write(pWidget, vParameter)) {
        const ConfigKey& key = getKey();
        qWarning() << "Property" << m_propertyName
                   << "was not defined for widget" << pWidget
                   << "(parameter:" << parameter << ")"
                   << key.group << key.item;
        return;
    }

    m_propertyValue = vParameter;

    pWidget->style()->polish(pWidget);

    // These calls don't always trigger the repaint, so call it explicitly.
    pWidget->repaint();
}
