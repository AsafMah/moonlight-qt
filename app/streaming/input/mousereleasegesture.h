#pragma once

class MouseReleaseGesture
{
public:
    void hold(bool held) { m_Held = held; }
    bool isFree() const { return m_Held; }
    void focusLost() { m_Held = false; }

private:
    bool m_Held = false;
};
