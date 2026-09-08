/**
 * @brief Editing commands shared by native pop-out shortcuts and menus.
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef FS_NATIVE_POPOUT_EDIT_H
#define FS_NATIVE_POPOUT_EDIT_H

#include <string>
#include "llpreprocessor.h"
#include "indra_constants.h"

namespace FSNativePopoutEdit
{
enum class Command { None, Undo, Redo, Cut, Copy, Paste, Delete, SelectAll };

inline bool isClipboardCommand(Command command)
{
    return command == Command::Cut || command == Command::Copy || command == Command::Paste;
}

inline Command fromKey(KEY key, MASK mask)
{
    if (mask == MASK_CONTROL)
    {
        switch (key)
        {
        case 'Z': return Command::Undo;
        case 'Y': return Command::Redo;
        case 'X': return Command::Cut;
        case 'C':
        case KEY_INSERT: return Command::Copy;
        case 'V': return Command::Paste;
        case 'A': return Command::SelectAll;
        }
    }
    else if (mask == (MASK_CONTROL | MASK_SHIFT) && key == 'Z')
    {
        return Command::Redo;
    }
    else if (mask == MASK_SHIFT)
    {
        if (key == KEY_INSERT) return Command::Paste;
        if (key == KEY_DELETE) return Command::Cut;
    }
    else if (mask == MASK_NONE && key == KEY_DELETE)
    {
        return Command::Delete;
    }
    return Command::None;
}

// LLTextEditor's edit interface is inherited through a protected base, unlike
// LLLineEditor's. Use their public methods rather than an unsafe interface cast
// or the global edit handler, which might refer to another viewer window.
template<typename Editor>
bool enabled(const Editor& editor, Command command)
{
    switch (command)
    {
    case Command::Undo: return editor.canUndo();
    case Command::Redo: return editor.canRedo();
    case Command::Cut: return editor.canCut();
    case Command::Copy: return editor.canCopy();
    case Command::Paste: return editor.canPaste();
    case Command::Delete: return editor.canDoDelete();
    case Command::SelectAll: return editor.canSelectAll();
    default: return false;
    }
}

template<typename Editor>
bool execute(Editor& editor, Command command)
{
    if (command == Command::None) return false;
    // Consume unavailable editing commands too: they must not fall through to
    // a different control or to a world action.
    if (!enabled(editor, command)) return true;
    switch (command)
    {
    case Command::Undo: editor.undo(); break;
    case Command::Redo: editor.redo(); break;
    case Command::Cut: editor.cut(); break;
    case Command::Copy: editor.copy(); break;
    case Command::Paste: editor.paste(); break;
    case Command::Delete: editor.doDelete(); break;
    case Command::SelectAll: editor.selectAll(); break;
    default: break;
    }
    return true;
}
}

#endif
