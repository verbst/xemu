/*
 * xemu Settings Management
 *
 * Copyright (C) 2025 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_CONTROLLERS_H
#define XEMU_CONTROLLERS_H

#include "xemu-input.h"
#include <SDL3/SDL.h>

enum class RebindEventResult {
    Ignore,
    Complete,
};

struct RebindingMap {
protected:
    int m_table_row;
    RebindingMap(int table_row) : m_table_row{ table_row }
    {
    }

public:
    virtual RebindEventResult ConsumeRebindEvent(SDL_Event *event) = 0;

    /*
     * Checked once per frame while a rebinding is outstanding, for devices
     * whose input does not arrive as an event. Devices that are driven by
     * events alone leave this alone.
     */
    virtual RebindEventResult Poll()
    {
        return RebindEventResult::Ignore;
    }

    /* Describes what the user is being waited on, for the table cell. */
    virtual const char *GetPrompt() const
    {
        return "Press a key to rebind";
    }

    int GetTableRow() const
    {
        return m_table_row;
    }

    virtual ~RebindingMap() = default;
};

struct ControllerKeyboardRebindingMap : public virtual RebindingMap {
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;

    ControllerKeyboardRebindingMap(int table_row) : RebindingMap(table_row)
    {
    }
};

class ControllerGamepadRebindingMap : public virtual RebindingMap {
    ControllerState *m_state;
    bool m_seen_key_down;

    RebindEventResult HandleButtonEvent(SDL_GamepadButtonEvent *event);
    RebindEventResult HandleAxisEvent(SDL_GamepadAxisEvent *event);

public:
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;
    ControllerGamepadRebindingMap(int table_row, ControllerState *state)
        : RebindingMap(table_row), m_state{ state }, m_seen_key_down{ false }
    {
    }
};

/*
 * A pad attached to a MiSTer, whose presses arrive in the video stream's own
 * packets rather than through SDL. There is no event to consume, so the choice
 * is polled from what the last packet carried.
 */
class ControllerGroovyRebindingMap : public virtual RebindingMap {
    ControllerState *m_state;
    bool m_wants_axis;

public:
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;
    RebindEventResult Poll() override;
    const char *GetPrompt() const override;
    ControllerGroovyRebindingMap(int table_row, ControllerState *state);
    ~ControllerGroovyRebindingMap() override;
};

#endif // XEMU_CONTROLLERS_H
