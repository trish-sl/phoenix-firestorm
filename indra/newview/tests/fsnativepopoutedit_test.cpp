/**
 * @brief Standalone regression tests for native pop-out editing commands.
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include "fsnativepopoutedit.h"
#include "fsnativepopoutpolicy.h"
#include <array>
#include <iostream>
#include <stdexcept>

using FSNativePopoutEdit::Command;

namespace
{
void check(bool value, const char* description)
{
    if (!value) throw std::runtime_error(description);
}

struct Editor
{
    std::array<bool, 8> allowed{};
    Command last = Command::None;
    int calls = 0;
    bool can(Command command) const { return allowed[static_cast<size_t>(command)]; }
    void run(Command command) { last = command; ++calls; }

    bool canUndo() const { return can(Command::Undo); }
    bool canRedo() const { return can(Command::Redo); }
    bool canCut() const { return can(Command::Cut); }
    bool canCopy() const { return can(Command::Copy); }
    bool canPaste() const { return can(Command::Paste); }
    bool canDoDelete() const { return can(Command::Delete); }
    bool canSelectAll() const { return can(Command::SelectAll); }
    void undo() { run(Command::Undo); }
    void redo() { run(Command::Redo); }
    void cut() { run(Command::Cut); }
    void copy() { run(Command::Copy); }
    void paste() { run(Command::Paste); }
    void doDelete() { run(Command::Delete); }
    void selectAll() { run(Command::SelectAll); }
};
}

int main()
{
    using namespace FSNativePopoutEdit;
    struct Shortcut { KEY key; MASK mask; Command command; };
    const Shortcut shortcuts[] = {
        { 'C', MASK_CONTROL, Command::Copy }, { 'V', MASK_CONTROL, Command::Paste },
        { 'X', MASK_CONTROL, Command::Cut }, { 'A', MASK_CONTROL, Command::SelectAll },
        { 'Z', MASK_CONTROL, Command::Undo }, { 'Y', MASK_CONTROL, Command::Redo },
        { 'Z', MASK_CONTROL | MASK_SHIFT, Command::Redo },
        { KEY_INSERT, MASK_CONTROL, Command::Copy }, { KEY_INSERT, MASK_SHIFT, Command::Paste },
        { KEY_DELETE, MASK_SHIFT, Command::Cut }, { KEY_DELETE, MASK_NONE, Command::Delete }
    };
    for (const auto& shortcut : shortcuts)
    {
        check(fromKey(shortcut.key, shortcut.mask) == shortcut.command, "Wrong shortcut mapping");
        Editor focused;
        focused.allowed[static_cast<size_t>(shortcut.command)] = true;
        check(execute(focused, fromKey(shortcut.key, shortcut.mask)), "Editing command not consumed");
        check(focused.last == shortcut.command && focused.calls == 1, "Wrong or repeated editor operation");

        // Empty clipboard, no selection, read-only editor, or unavailable undo.
        focused.allowed.fill(false);
        check(!enabled(focused, shortcut.command), "Unavailable command enabled");
        check(execute(focused, shortcut.command), "Unavailable command fell through");
        check(focused.calls == 1, "Unavailable command executed");
    }
    for (KEY key : { KEY('V'), KEY('C'), KEY('Z'), KEY_RETURN, KEY_BACKSPACE, KEY_LEFT })
    {
        check(fromKey(key, MASK_NONE) == Command::None, "Ordinary typing intercepted");
        check(fromKey(key, MASK_CONTROL | MASK_ALT) == Command::None, "AltGr intercepted");
    }
    check(fromKey(KEY_DELETE, MASK_CONTROL) == Command::None, "Word deletion intercepted");
    check(fromKey('V', MASK_CONTROL | MASK_SHIFT) == Command::None, "Unassigned shortcut intercepted");
    for (Command command : { Command::Cut, Command::Copy, Command::Paste })
    {
        check(isClipboardCommand(command), "Inventory clipboard shortcut missing");
    }
    for (Command command : { Command::None, Command::Delete, Command::SelectAll, Command::Undo, Command::Redo })
    {
        check(!isClipboardCommand(command), "Inventory panel command intercepted");
    }
    using namespace FSNativePopoutPolicy;
    check(isToggleShortcut(KEY_F12, MASK_CONTROL | MASK_SHIFT), "Toggle shortcut missing");
    check(!isToggleShortcut('P', MASK_CONTROL), "Preferences shortcut intercepted");
    check(!isToggleShortcut(KEY_F12, MASK_NONE), "Plain F12 intercepted");
    check(!isToggleShortcut(KEY_F12, MASK_CONTROL | MASK_SHIFT | MASK_ALT), "Alt shortcut intercepted");
    for (const char* name : { "area_search", "notification_well_window", "preferences",
        "settings_debug", "inventory",
        "secondary_inventory", "preview_script", "preview_scriptedit", "preview_notecard" })
    {
        check(find(name) != nullptr, "Requested floater unsupported");
    }
    check(find("notification_well_window")->setting != nullptr, "Notification well missing singleton toggle");
    check(!find("unknown_floater"), "Arbitrary floater allowed");
    check(find("preview_notecard")->setting == nullptr, "Document gets a singleton toggle");
    Editor editor;
    check(!execute(editor, Command::None) && editor.calls == 0, "Non-edit key consumed");
    std::cout << "PASS: editing commands, key passthrough, pop-out shortcut, and floater policy\n";
}
