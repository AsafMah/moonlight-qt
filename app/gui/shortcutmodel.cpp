#include "shortcutmodel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>

ShortcutModel::ShortcutModel(QObject* parent)
    : QAbstractListModel(parent), m_Profile(ShortcutProfile::loadFromSettings()),
#ifdef Q_OS_WIN32
      m_ModifierRecorder(true)
#else
      m_ModifierRecorder(false)
#endif
{
    for (const ShortcutAction& action : m_Profile.registry().actions()) {
        if (action.id != QLatin1String(ShortcutActions::RightAltMouseRelease)) {
            m_VisibleActions.append(action);
        }
    }
}

ShortcutModel::~ShortcutModel()
{
    cancelCapture();
}

int ShortcutModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_VisibleActions.size();
}

const ShortcutAction* ShortcutModel::actionAt(int row) const
{
    const QVector<ShortcutAction>& actions = m_VisibleActions;
    if (row < 0 || row >= actions.size()) {
        return nullptr;
    }
    return &actions[row];
}

QString ShortcutModel::conflictText(const QString& actionId, int slot) const
{
    QStringList names;
    for (const QString& otherId : m_Profile.conflictingActions(actionId, slot)) {
        const ShortcutAction* other = m_Profile.registry().find(otherId);
        if (other != nullptr) {
            names.append(other->translatedName());
        }
    }

    if (names.isEmpty()) {
        return QString();
    }
    return tr("Also used by: %1").arg(names.join(QStringLiteral(", ")));
}

QVariant ShortcutModel::data(const QModelIndex& index, int role) const
{
    const ShortcutAction* action = actionAt(index.row());
    if (!index.isValid() || action == nullptr) {
        return QVariant();
    }

    switch (role) {
    case ActionIdRole:
        return action->id;
    case Qt::DisplayRole:
    case NameRole:
        return action->translatedName();
    case GroupRole:
        return action->translatedGroup();
    case FirstInGroupRole: {
        const ShortcutAction* previous = actionAt(index.row() - 1);
        return previous == nullptr || qstrcmp(previous->group, action->group) != 0;
    }
    case Binding1Role:
    case Binding2Role:
        return m_Profile.bindings(action->id)[role - Binding1Role].toDisplayString();
    case Conflict1Role:
    case Conflict2Role:
        return conflictText(action->id, role - Conflict1Role);
    case CustomizedRole:
        return m_Profile.isCustomized(action->id);
    case HoldRole:
        return action->hold;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ShortcutModel::roleNames() const
{
    QHash<int, QByteArray> roles;

    roles[ActionIdRole] = "actionId";
    roles[NameRole] = "name";
    roles[GroupRole] = "group";
    roles[FirstInGroupRole] = "firstInGroup";
    roles[Binding1Role] = "binding1";
    roles[Binding2Role] = "binding2";
    roles[Conflict1Role] = "conflict1";
    roles[Conflict2Role] = "conflict2";
    roles[CustomizedRole] = "customized";
    roles[HoldRole] = "hold";

    return roles;
}

ShortcutModel::DefaultPrefix ShortcutModel::defaultPrefix() const
{
    return static_cast<DefaultPrefix>(m_Profile.defaultPrefix());
}

void ShortcutModel::setDefaultPrefix(DefaultPrefix prefix)
{
    if (prefix == defaultPrefix()) {
        return;
    }

    m_Profile.setDefaultPrefix(static_cast<ShortcutProfile::DefaultPrefix>(prefix));
    emit defaultPrefixChanged();
    commit();
}

int ShortcutModel::conflictCount() const
{
    return m_Profile.conflicts().size();
}

void ShortcutModel::setRightAltReleaseEnabled(bool enabled)
{
    m_Profile.setRightAltReleaseEnabled(enabled);
    commit();
}

int ShortcutModel::captureKey(int row, int slot, int qtKey, int qtModifiers, quint32 nativeScanCode)
{
    const ShortcutAction* action = actionAt(row);
    if (action == nullptr) {
        return CaptureCancelled;
    }

    const int scancode = ShortcutKeys::fromQtKeyEvent(qtKey, nativeScanCode);
    if (row != m_RecorderRow || slot != m_RecorderSlot) {
        if (m_CaptureFilterInstalled && QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->removeEventFilter(this);
            m_CaptureFilterInstalled = false;
        }
        m_ModifierRecorder.reset();
        m_RecorderRow = row;
        m_RecorderSlot = slot;
    }
    if (action->hold) {
        // A modifier may become the binding when it comes up, see
        // captureKeyRelease()
        m_ModifierRecorder.press(scancode, ShortcutBinding::fromQtModifiers(qtModifiers));
        if (!m_CaptureFilterInstalled && QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->installEventFilter(this);
            m_CaptureFilterInstalled = true;
        }
    }

    switch (qtKey) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
        return CaptureIgnored;
    default:
        if (ShortcutKeys::isModifierKey(scancode)) {
            return CaptureIgnored;
        }
        break;
    }

    const quint8 modifiers = ShortcutBinding::fromQtModifiers(qtModifiers);
    if (modifiers == ShortcutBinding::ModNone) {
        if (qtKey == Qt::Key_Escape) {
            finishCapture(row, slot);
            return CaptureCancelled;
        } else if (qtKey == Qt::Key_Backspace || qtKey == Qt::Key_Delete) {
            clearBinding(row, slot);
            finishCapture(row, slot);
            return CaptureCleared;
        }
    }

    const ShortcutBinding binding(modifiers, scancode);
    if (!action->accepts(binding) || !m_Profile.setBinding(action->id, slot, binding)) {
        return CaptureInvalid;
    }

    commit();
    finishCapture(row, slot);
    return CaptureAccepted;
}

int ShortcutModel::captureKeyRelease(int row, int slot, int qtKey, quint32 nativeScanCode)
{
    const ShortcutAction* action = actionAt(row);
    if (action == nullptr || !action->hold || row != m_RecorderRow || slot != m_RecorderSlot) {
        return CaptureIgnored;
    }

    const ShortcutBinding binding =
        m_ModifierRecorder.release(ShortcutKeys::fromQtKeyEvent(qtKey, nativeScanCode));
    if (binding.isNull()) {
        return CaptureIgnored;
    }
    if (!m_Profile.setBinding(action->id, slot, binding)) {
        return CaptureInvalid;
    }

    commit();
    finishCapture(row, slot);
    return CaptureAccepted;
}

void ShortcutModel::cancelCapture()
{
    if (m_CaptureFilterInstalled && QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    m_CaptureFilterInstalled = false;
    m_RecorderRow = -1;
    m_RecorderSlot = -1;
    m_ModifierRecorder.reset();
}

void ShortcutModel::finishCapture(int row, int slot)
{
    if (row != m_RecorderRow || slot != m_RecorderSlot) {
        return;
    }
    cancelCapture();
    emit captureFinished(row, slot);
}

bool ShortcutModel::eventFilter(QObject* watched, QEvent* event)
{
    Q_UNUSED(watched);
    if (!m_CaptureFilterInstalled) {
        return QAbstractListModel::eventFilter(watched, event);
    }

    if (event->type() == QEvent::ApplicationDeactivate ||
        event->type() == QEvent::MouseButtonPress) {
        cancelCapture();
        return false;
    }

    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }

    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (!keyEvent->isAutoRepeat()) {
            const int result = captureKey(m_RecorderRow, m_RecorderSlot, keyEvent->key(),
                                          keyEvent->modifiers(), keyEvent->nativeScanCode());
            if (result == CaptureInvalid) {
                emit captureRejected(m_RecorderRow, m_RecorderSlot);
            }
        }
        return true;
    }

    if (event->type() == QEvent::KeyRelease) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (!keyEvent->isAutoRepeat()) {
            const int row = m_RecorderRow;
            const int slot = m_RecorderSlot;
            const int result =
                captureKeyRelease(row, slot, keyEvent->key(), keyEvent->nativeScanCode());
            if (result == CaptureInvalid) {
                emit captureRejected(row, slot);
            }
        }
        return true;
    }

    return QAbstractListModel::eventFilter(watched, event);
}

void ShortcutModel::clearBinding(int row, int slot)
{
    const ShortcutAction* action = actionAt(row);
    if (action != nullptr && m_Profile.setBinding(action->id, slot, ShortcutBinding())) {
        commit();
    }
}

void ShortcutModel::resetAction(int row)
{
    const ShortcutAction* action = actionAt(row);
    if (action != nullptr) {
        m_Profile.resetAction(action->id);
        commit();
    }
}

void ShortcutModel::resetAll()
{
    const DefaultPrefix oldPrefix = defaultPrefix();

    m_Profile.resetAll();
    if (oldPrefix != defaultPrefix()) {
        emit defaultPrefixChanged();
    }
    commit();
}

QString ShortcutModel::bindingText(const QString& actionId) const
{
    QStringList texts;
    for (const ShortcutBinding& binding : m_Profile.bindings(actionId)) {
        if (!binding.isNull()) {
            texts.append(binding.toDisplayString());
        }
    }
    return texts.join(QStringLiteral(" / "));
}

void ShortcutModel::commit()
{
    m_Profile.saveToSettings();

    // Conflicts and defaults can change on any row, so refresh them all
    if (rowCount() > 0) {
        emit dataChanged(index(0), index(rowCount() - 1));
    }
    emit bindingsChanged();
}
