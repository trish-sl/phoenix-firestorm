# Experimental desktop pop-outs (Windows)

This prototype hosts selected **existing floater instances** in separate Windows
desktop windows. It is disabled by default and does not persist across launches.

Click inside a supported floater and press **Ctrl+Shift+F12** to pop it out or
return it to the viewer. Ctrl+P retains its existing Preferences action. The
shortcut supports nearby chat, world map, Radar, Area Search, script editors
(inventory and in-object scripts), notecard editors, Preferences, Debug Settings,
Inventory and additional inventory windows. It also supports Gestures,
Statistics, People, Contacts, sound previews, Inventory Settings, and Region Tracker.

Each document is moved independently without reopening or reloading it. Its
native title follows document title changes. Desktop position and size are
remembered for that floater instance during the session; returning restores its
viewer rectangle/tab. Restored desktop positions are constrained to an available
monitor. Quit returns detached editors before the normal unsaved-change checks.

## Trying it

Build the viewer normally from this source. Log in in windowed mode, open
**Advanced > Show Debug Settings**, and enable the desired setting:

| Setting | Existing floater |
| --- | --- |
| `FSExperimentalPopoutChat` | Nearby chat, including its existing input/send controls |
| `FSExperimentalPopoutMap` | World map, including local mouse-drag panning |
| `FSExperimentalPopoutNearby` | Nearby people through Firestorm's Radar |
| `FSExperimentalPopoutAreaSearch` | Area Search |
| `FSExperimentalPopoutPreferences` | Preferences |
| `FSExperimentalPopoutSettings` | Debug Settings |
| `FSExperimentalPopoutInventory` | Main inventory |

Drag the native title bar to move a desktop window onto another monitor.
Clicking its X closes the floater through its normal close behavior, including
unsaved-change prompts. Its debug setting remains enabled, but the host waits
until you open the floater again before popping it out. This also works when
closing destroys the floater instance. Set the setting to false or press
Ctrl+Shift+F12 to return an open floater to the main viewer.
An existing toolbar action that hides or rehosts the floater also retires its
native window. Graphics resets, fullscreen transitions, and disconnection
return the floaters and disable their toggles.

## Implementation and merge footprint

The implementation lives in `indra/newview/fsnativepopout.cpp` and its small
header. Existing-source integration consists of the CMake entries, seven
nonpersistent settings, main-loop update/draw and pre-save shutdown calls in
`llappviewer.cpp`, and a focused-floater keyboard hook plus UI-shutdown/graphics-reset calls in `llviewerwindow.cpp`. There are no edits to
`llui`, `llwindow`, the chat/map/Radar implementations, or their XUI layouts.
Other platforms compile no-op entry points.

Each host owns a native HWND and an isolated floater root. The registered
floater is reparented, not copied or reconstructed. The registry's show
validators run before detachment; hidden/destroyed/rehosted floaters retire
their native windows without being reopened. Regular chat processing and
Radar/map data updates remain in their existing implementations.

Rendering uses the viewer's current context and an offscreen render target.
BGRA pixels are copied to a native DIB at **at most 10 frames per second per
window**, with a **2048-pixel limit per surface dimension**. This deliberately
avoids adding shared GL contexts and changing platform backends. Readback can
stall the GPU; this is a feasibility transport, not a production performance
solution. The native windows are painted even when the main viewer's normal
draw path returns early, although background throttling still applies.

Native events are dispatched on the viewer thread. UI-root, floater-root,
scale, and application-focus substitutions are scoped to synchronous dispatch
and rendering. Satellite mouse capture is kept out of the main viewer's
hover/world-picking loop. World map panning uses `translatePan()` so it does not
warp the main viewer's cursor. Input is blocked during viewer modal dialogs
and disconnection. Surfaces are released before graphics-context teardown.

Text editors handle Ctrl+C/V/X/A/Z/Y, Ctrl+Shift+Z, Ctrl+Insert, Shift+Insert,
Shift+Delete, and Delete through their own editing methods. Clipboard contents
still pass through the normal viewer paste code, including text length and
read-only checks. The host remembers each window's focused field when switching
away and restores it on return. Tab/Shift+Tab fall back to floater navigation
when the focused control does not consume them.

Inventory panes also route Cut/Copy/Paste (including Ctrl+Insert and
Shift+Insert) through their own clipboard methods. The target must belong to
the focused, visible inventory panel in that native window. Search/rename fields
retain text editing, and inventory Delete/navigation retain their panel handlers.

Right-clicking a text editor opens a native edit menu. Right-clicking an inventory
tree offers Cut/Copy/Paste for its **current selection**, preserving that selection. The context-menu key and
Shift+F10 open the same menu for the focused editor. Menu commands use the same
capability checks as keyboard shortcuts; they never target the global Edit
menu handler belonging to another window. Mouse-capture cleanup clears its stored
owner before invoking callbacks, preventing recursive capture-loss notification.

## Prototype limitations

- **General menus, tooltips, and popup lists are not detached.** Text editors
  and inventory clipboard actions have native edit menus; other right-click actions remain unsupported.
  Menus opened by buttons, combo boxes, emoji pickers, and
  secondary dialogs can still appear in the main viewer and may have incorrect
  placement. Return the floater to use those interactions normally.
- Text entry forwards Unicode characters (including surrogate pairs), ordinary
  key events, and modifier masks. IME composition/candidate-window placement,
  accessibility, and full keyboard navigation are not implemented as native
  window features. Existing text controls still provide their ordinary editing
  behavior where it works through these events.
- Native windows follow the main viewer's UI scale. Independent per-monitor
  DPI/font scaling and desktop positions across viewer launches are not implemented.
- Native move/resize operations and edit menus run on the viewer thread. Their
  Windows modal loops can pause viewer progress until the operation ends.
- Cross-window drag-and-drop is not supported. No separate viewer session,
  network connection, or world render is created.
- Main-viewer background throttling, AFK, audio, and activation policies are
  unchanged. Behavior with the main viewer minimized needs runtime testing.
- This does not promise that other floaters can be detached safely; the small
  explicit allowlist is intentional.

## Validation and next checks

Runtime behavior has **not** been validated in a logged-in viewer. Do not treat
this as a completed production feature. The normal build attempt stopped
during CMake regeneration because existing WebSocket++ dependency headers were
inaccessible, before compiling the new source.
A targeted MSVC syntax check using the existing Release precompiled header also
stopped in the map's dependency chain at inaccessible `glm/gtc/type_ptr.hpp`.
Consequently the complete source has not passed compilation or linking.
A separate MSVC syntax check of the host/manager against real viewer headers,
excluding the existing map mouse handler and its unavailable dependency, passes.
This is only a partial compilation check.

The standalone editing-command regression test covers shortcut mappings,
capability checks, single execution, ordinary/AltGr key passthrough, the toggle
shortcut, and the floater allowlist. It
passes with MSVC warnings treated as errors. The helper also passes a targeted
syntax check instantiated against the real `LLTextEditor` and `LLLineEditor`
APIs. These checks do not exercise the OS clipboard or native focus/menu events. From an x64
Visual Studio developer command prompt at the repository root, run:

```bat
cl /nologo /EHsc /std:c++20 /W4 /WX /MT /DLL_WINDOWS=1 /Iindra/llcommon /Iindra/newview indra/newview/tests/fsnativepopoutedit_test.cpp /Fobuild-vc170-64/nativepopoutedit_test.obj /Febuild-vc170-64/nativepopoutedit_test.exe
build-vc170-64\nativepopoutedit_test.exe
```

For a runtime pass, test each window separately before opening several together:

1. Detach, resize, move to another monitor, close, and repeat. Confirm that the
   original floater/tab and its geometry survive reattachment.
2. Receive nearby chat and send a harmless test line; verify that it appears
   once and uses the ordinary chat processing. Exercise selection, copy/paste,
   arrows, backspace, Enter, and supplementary Unicode characters. Copy a URL
   in a browser, Alt+Tab back, and paste into the remembered chat input. Repeat
   with Shift+Insert and the native right-click menu. Confirm paste does not send
   chat until Enter/Send, copy works back into the browser, read-only history
   cannot be edited, and shortcuts do not modify another window's editor.
3. Pan, zoom, search, and select on the world map. Verify that dragging does not
   move the main-window cursor or start world interaction.
4. Check Radar updates and selection; open a profile and verify that the
   secondary window remains in the main viewer.
5. Switch focus repeatedly, minimize/restore either window, and close the main
   viewer with pop-outs open. Verify no stale keyboard/mouse capture or crash.
6. Exercise UI-scale changes, graphics resets, fullscreen transitions,
   disconnection, and applicable RLVa restrictions while detached. Verify that
   closing/restricting a floater never reopens it through the native host.
7. Open two unsaved scripts and two notecards. Toggle each independently; check
   selection, clipboard, document text, save actions, and return to the original
   tab. Quit and cancel through the normal unsaved-change prompts.
8. Exercise Preferences Apply/Cancel and a graphics reset while detached. Check
   inventory searching and opening items, and Area Search result updates. Menus
   and drag-and-drop retain the limitations above; return with the shortcut to
   use them. Test shortcut repeat, desktop position restoration, and Ctrl+P.
9. Compare frame time and GPU stalls with zero, one, and three pop-outs open.
   Check main-viewer rendering for scissor, blend, font, or viewport corruption.

The next architectural decision should follow these runtime results: whether
to retain the offscreen host with a better presentation path, and which UI
services need explicit window ownership to make menus/input complete.

Additional inventory regression checks: select different items in two detached
inventory windows, switch focus/tabs, and copy/paste between them. Verify that
search and rename fields edit text, Delete retains normal inventory behavior,
and right-click/Shift+F10 operates on the existing selection. Repeat after closing
a window and during modal prompts. These native runtime checks remain pending.
