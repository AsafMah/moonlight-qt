#include "hostkeys.h"

#include <Limelight.h>
#include "SDL_compat.h"

static_assert(HostKeys::HostShift == MODIFIER_SHIFT && HostKeys::HostCtrl == MODIFIER_CTRL &&
                  HostKeys::HostAlt == MODIFIER_ALT && HostKeys::HostMeta == MODIFIER_META &&
                  HostKeys::HostExtended == MODIFIER_EXTENDED,
              "HostKeys modifier bits must match moonlight-common-c");

bool HostKeys::send(const ShortcutBinding& binding)
{
    const QVector<KeyStroke> keyStrokes = strokes(binding);
    if (keyStrokes.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot send key combo to host: %s",
                    qPrintable(binding.toString()));
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Sending key combo to host: %s",
                qPrintable(displayText(binding)));

    bool ok = true;
    for (const KeyStroke& stroke : keyStrokes) {
        const int rc = LiSendKeyboardEvent2(short(0x8000 | stroke.keyCode),
                                            stroke.down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                            char(stroke.modifiers), 0);
        if (rc != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "LiSendKeyboardEvent2 failed: rc=%d action=%s vk=0x%04x", rc,
                        stroke.down ? "down" : "up", (int)(0x8000 | stroke.keyCode));
            ok = false;
        }
    }
    return ok;
}
