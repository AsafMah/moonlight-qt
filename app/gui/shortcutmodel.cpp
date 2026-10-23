#include "shortcutmodel.h"

ShortcutModel::ShortcutModel(QObject* parent)
    : QAbstractListModel(parent),
      m_Profile(ShortcutProfile::loadFromSettings())
{
}

int ShortcutModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_Profile.registry().actions().size();
}

const ShortcutAction* ShortcutModel::actionAt(int row) const
{
    const QVector<ShortcutAction>& actions = m_Profile.registry().actions();
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
    case FirstInGroupRole:
    {
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

int ShortcutModel::captureKey(int row, int slot, int qtKey, int qtModifiers,
                              quint32 nativeScanCode)
{
    const ShortcutAction* action = actionAt(row);
    if (action == nullptr) {
        return CaptureCancelled;
    }

    const int scancode = ShortcutKeys::fromQtKeyEvent(qtKey, nativeScanCode);
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
            return CaptureCancelled;
        }
        else if (qtKey == Qt::Key_Backspace || qtKey == Qt::Key_Delete) {
            clearBinding(row, slot);
            return CaptureCleared;
        }
    }

    const ShortcutBinding binding(modifiers, scancode);
    if (!binding.isValid() || !m_Profile.setBinding(action->id, slot, binding)) {
        return CaptureInvalid;
    }

    commit();
    return CaptureAccepted;
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
