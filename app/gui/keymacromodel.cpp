#include "keymacromodel.h"

KeyMacroModel::KeyMacroModel(QObject* parent)
    : QAbstractListModel(parent), m_Macros(HostKeys::loadMacrosFromSettings())
{
}

int KeyMacroModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_Macros.size();
}

QVariant KeyMacroModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_Macros.size()) {
        return QVariant();
    }

    const HostKeys::Macro& macro = m_Macros[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return macro.name;
    case KeysRole:
        return HostKeys::displayText(macro.binding);
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> KeyMacroModel::roleNames() const
{
    QHash<int, QByteArray> roles;

    roles[NameRole] = "name";
    roles[KeysRole] = "keys";

    return roles;
}

QString KeyMacroModel::pendingKeys() const
{
    return HostKeys::displayText(m_Pending);
}

int KeyMacroModel::captureKey(int qtKey, int qtModifiers, quint32 nativeScanCode)
{
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
        } else if (qtKey == Qt::Key_Backspace || qtKey == Qt::Key_Delete) {
            setPending(ShortcutBinding());
            return CaptureCleared;
        }
    }

    // Unlike shortcuts, macros may use any modifiers or none at all
    const ShortcutBinding binding(modifiers, scancode);
    if (!HostKeys::canSend(binding)) {
        return CaptureInvalid;
    }

    setPending(binding);
    return CaptureAccepted;
}

bool KeyMacroModel::addMacro(const QString& name)
{
    const HostKeys::Macro macro{ HostKeys::normalizedName(name), m_Pending };
    if (!canAdd() || !HostKeys::isValidMacro(macro)) {
        return false;
    }

    beginInsertRows(QModelIndex(), m_Macros.size(), m_Macros.size());
    m_Macros.append(macro);
    endInsertRows();

    HostKeys::saveMacrosToSettings(m_Macros);
    setPending(ShortcutBinding());
    emit macrosChanged();
    return true;
}

void KeyMacroModel::removeMacro(int row)
{
    if (row < 0 || row >= m_Macros.size()) {
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_Macros.remove(row);
    endRemoveRows();

    HostKeys::saveMacrosToSettings(m_Macros);
    emit macrosChanged();
}

void KeyMacroModel::setPending(const ShortcutBinding& binding)
{
    if (binding != m_Pending) {
        m_Pending = binding;
        emit pendingKeysChanged();
    }
}
