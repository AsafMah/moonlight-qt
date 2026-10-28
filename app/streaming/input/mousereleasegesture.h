#pragma once

#include <QtGlobal>

class MouseReleaseGesture
{
public:
    static constexpr qint64 TapDurationMs = 250;
    static constexpr qint64 DoubleTapGapMs = 300;

    void hold(bool held, qint64 now)
    {
        if (held) {
            if (!m_Held) {
                m_PressTime = now;
                m_Held = true;
            }
            return;
        }
        if (!m_Held) {
            return;
        }
        m_Held = false;
        if (now - m_PressTime > TapDurationMs) {
            m_LastTap = -1;
        } else if (m_LastTap >= 0 && m_PressTime - m_LastTap <= DoubleTapGapMs) {
            m_Persistent = !m_Persistent;
            m_LastTap = -1;
        } else {
            m_LastTap = now;
        }
    }
    bool escape(bool pressed)
    {
        if (pressed && m_Persistent) {
            reset();
            m_EscapeSwallowed = true;
        }
        const bool consumed = m_EscapeSwallowed;
        if (!pressed) {
            m_EscapeSwallowed = false;
        }
        return consumed;
    }
    bool isFree() const { return m_Held || m_Persistent; }
    bool isPersistent() const { return m_Persistent; }
    void focusLost()
    {
        m_Held = m_EscapeSwallowed = false;
        m_LastTap = -1;
    }
    void reset()
    {
        m_Held = m_Persistent = m_EscapeSwallowed = false;
        m_LastTap = -1;
    }

private:
    bool m_Held = false;
    bool m_Persistent = false;
    bool m_EscapeSwallowed = false;
    qint64 m_PressTime = 0;
    qint64 m_LastTap = -1;
};
