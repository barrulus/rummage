# Accessibility and input methods

Rummage's native Wayland interface exposes an AT-SPI accessibility tree for screen
readers such as Orca. Enable your desktop's screen reader normally; it can discover
Rummage after the file manager has already started. The bridge uses the desktop's
accessibility bus and does not require a GTK or Qt interface. `NO_AT_BRIDGE=1`
disables accessibility, as it does for other AT-SPI applications.

Visible files have names, selection and focus states, and an activation action.
Dialogs, menu commands, tabs and text controls expose their labels and actions.
Changes to focus, selection, text, status messages and errors generate events.
Editable controls support text and selection queries and edits using Unicode
character offsets. Password controls expose masked text and refuse accessibility
clipboard extraction.

The tree follows the visible portion of the interface. Use the normal keyboard
navigation to scroll through larger folders and expose further entries. Wayland
does not give clients a global screen position, so component coordinates are
available relative to the window; global positions are reported as unavailable.
The accessibility interface is still being expanded: not every decorative control
or custom preview provides the same semantic detail as the file list and editors.

## Composing text

On compositors that offer `text-input-v3`, Rummage connects the active editor to
the desktop input method. This covers file renaming, location and search fields,
picker filenames and dialog/settings text fields. Composition appears underlined
inside the editor; choosing a candidate commits it to the text. The candidate
window is positioned at the caret, including with fractional display scaling and
application zoom.

Changing editor or window focus resets composition. IME navigation and candidate
selection do not run file-manager commands while composition is active. Password
fields tell the input method that the content is sensitive and send no surrounding
password text. Other fields send a bounded window of surrounding UTF-8 text.

On compositors without this protocol, ordinary keyboard input, XKB dead keys and
Compose sequences remain available. An input method must also be installed and
configured in the desktop session; Rummage does not start or configure one.

See [keyboard browsing](shortcuts.md#browse-entirely-with-the-keyboard) for the
navigation commands and [troubleshooting](troubleshooting.md) for reports.
