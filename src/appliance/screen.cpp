// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen.h"

namespace multisite_player {

ScreenAction screen_action(IdleMode mode, bool frames_arriving,
                           bool holding_picture, bool has_frame) {
    // A picture is arriving: there is an event on, and nothing here should
    // touch the screen.
    if (frames_arriving) return ScreenAction::Leave;

    // Hold is an operator act, not an idle state. Whoever pressed it is asking
    // for the frame in front of them to stay, so the idle screen must not take
    // it away — whatever idle mode the box is set to.
    //
    // It used to, because holding the picture was recognised only when the idle
    // mode itself was "hold". On a box left on the default idle screen, then,
    // pressing Hold replaced the picture with the box's own address and state:
    // the operator asks to freeze a frame and the screen answers with a splash,
    // which reads as the player having died. The idle screen is for having
    // nothing to show — waiting for the main site, or stopped — and a held
    // picture is neither.
    if (holding_picture && has_frame) return ScreenAction::Leave;

    // "Hold the last picture" as an idle mode is a different thing: frames have
    // stopped and nobody asked to stop, so the last frame simply stays up.
    if (mode == IdleMode::HoldFrame && has_frame) return ScreenAction::Leave;

    // Nothing on screen but an idle one. Stop lands here, and stopping is a
    // deliberate act, so the screen says what the box is doing rather than
    // keeping the last frame of an event up for ever.
    switch (mode) {
    case IdleMode::Black: return ScreenAction::Blank;
    case IdleMode::Image: return ScreenAction::Still;
    // Nothing has been decoded, so there is no frame to hold: the identity
    // screen at least says where the box is.
    case IdleMode::HoldFrame:
    case IdleMode::Splash: return ScreenAction::Splash;
    }
    return ScreenAction::Splash;
}

} // namespace multisite_player
