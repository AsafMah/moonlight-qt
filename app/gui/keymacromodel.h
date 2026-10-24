#pragma once

#include "streaming/input/hostkeys.h"

#include <QAbstractListModel>

// Exposes the custom key macros (HostKeys::Macro) to QML, one row per macro,
// plus a pending key combination captured for the next macro. Every change is
// saved to settings immediately; the in-stream menu reads them when it opens.
class KeyMacroModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int maxMacros READ maxMacros CONSTANT)
    Q_PROPERTY(int maxNameLength READ maxNameLength CONSTANT)
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY macrosChanged)
    Q_PROPERTY(QString pendingKeys READ pendingKeys NOTIFY pendingKeysChanged)

public:
    enum Roles
    {
        NameRole = Qt::UserRole + 1,
        KeysRole,
    };

    // Same meaning as ShortcutModel::CaptureResult
    enum CaptureResult
    {
        // A modifier on its own; keep waiting for the key
        CaptureIgnored,
        CaptureAccepted,
        // Cannot be sent to the host; keep waiting
        CaptureInvalid,
        CaptureCancelled,
        CaptureCleared,
    };
    Q_ENUM(CaptureResult)

    explicit KeyMacroModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int maxMacros() const { return HostKeys::MaxMacros; }
    int maxNameLength() const { return HostKeys::MaxNameLength; }
    bool canAdd() const { return m_Macros.size() < HostKeys::MaxMacros; }

    // Host-side text of the captured combination, empty when none
    QString pendingKeys() const;

    // Handles a key press while capturing the combination for a new macro.
    // Esc cancels and Backspace or Delete clears, when pressed without
    // modifiers. Uses the same key conversion as the shortcut editor.
    Q_INVOKABLE int captureKey(int qtKey, int qtModifiers, quint32 nativeScanCode);

    // Adds a macro with the pending combination. Returns false when the name
    // is blank, nothing is pending or the list is full.
    Q_INVOKABLE bool addMacro(const QString& name);
    Q_INVOKABLE void removeMacro(int row);

signals:
    void macrosChanged();
    void pendingKeysChanged();

private:
    void setPending(const ShortcutBinding& binding);

    QVector<HostKeys::Macro> m_Macros;
    ShortcutBinding m_Pending;
};
