#pragma once

#include "streaming/input/shortcuts.h"

#include <QAbstractListModel>

// Exposes the keyboard shortcut profile to QML, one row per action.
// Every change is saved to settings immediately.
class ShortcutModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(DefaultPrefix defaultPrefix READ defaultPrefix WRITE setDefaultPrefix NOTIFY
                   defaultPrefixChanged)
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY bindingsChanged)

public:
    enum Roles
    {
        ActionIdRole = Qt::UserRole + 1,
        NameRole,
        GroupRole,
        FirstInGroupRole,
        Binding1Role,
        Binding2Role,
        Conflict1Role,
        Conflict2Role,
        CustomizedRole,
        // Fires while held (press and release); may be bound to a modifier alone
        HoldRole,
    };

    enum DefaultPrefix
    {
        PrefixCtrlAltShift = ShortcutProfile::PrefixCtrlAltShift,
        PrefixCtrlAlt = ShortcutProfile::PrefixCtrlAlt,
    };
    Q_ENUM(DefaultPrefix)

    enum CaptureResult
    {
        // A modifier on its own; keep waiting for the key
        CaptureIgnored,
        CaptureAccepted,
        // Not allowed as a shortcut; keep waiting
        CaptureInvalid,
        CaptureCancelled,
        CaptureCleared,
    };
    Q_ENUM(CaptureResult)

    explicit ShortcutModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    DefaultPrefix defaultPrefix() const;
    void setDefaultPrefix(DefaultPrefix prefix);

    int conflictCount() const;

    // Handles a key press while capturing a binding for (row, slot).
    // Esc cancels and Backspace or Delete clears, when pressed without modifiers.
    Q_INVOKABLE int captureKey(int row, int slot, int qtKey, int qtModifiers,
                               quint32 nativeScanCode);

    // Handles a key release while capturing. A hold action is bound to a
    // modifier key on its own when that key comes up with no other key
    // pressed in between.
    Q_INVOKABLE int captureKeyRelease(int row, int slot, int qtKey, quint32 nativeScanCode);

    Q_INVOKABLE void clearBinding(int row, int slot);
    Q_INVOKABLE void resetAction(int row);
    Q_INVOKABLE void resetAll();

    // Display text of an action's bindings, e.g. "Ctrl+Alt+Shift+S" (empty if none)
    Q_INVOKABLE QString bindingText(const QString& actionId) const;

signals:
    void defaultPrefixChanged();
    void bindingsChanged();

private:
    const ShortcutAction* actionAt(int row) const;
    QString conflictText(const QString& actionId, int slot) const;
    void commit();

    ShortcutProfile m_Profile;
    ModifierKeyRecorder m_ModifierRecorder;
    int m_RecorderRow = -1;
    int m_RecorderSlot = -1;
};
