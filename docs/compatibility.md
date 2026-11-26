# Desktop compatibility

[Documentation](README.md) · [Installation](install.md) · [File picker](file-picker.md)

rummage requires a Linux Wayland session with xdg-shell and EGL/OpenGL ES 3.0.
Optional protocols and services provide desktop integration with the fallbacks
below. Available integration depends on the protocols and services provided by
your desktop session.

## Protocols and fallbacks

| Feature | Protocol/service | Without it |
| --- | --- | --- |
| Title bar | xdg-decoration in server-side mode | rummage draws client-side decorations |
| Cursor shapes | cursor-shape-v1 | Xcursor theme fallback using `XCURSOR_THEME` / `XCURSOR_SIZE` |
| Fractional scaling | fractional-scale-v1 and viewporter | No fractional-scale path; renders at scale 1 |
| Desktop clipboard and drag/drop | wl_data_device_manager | No cross-application file exchange through that protocol |
| Focus for launched applications | xdg-activation-v1 | Applications may open without focus |
| Desktop colours | Settings portal, `org.freedesktop.appearance` | Built-in palette unless `colors.toml` is supplied |
| Picker parent attachment | xdg-foreign-v2 | Independent picker window |
| Picker modal hint | xdg-dialog-v1 | Parenting can work without the modal hint |
| Device mount/eject | udisks2 | No udisks2 operations; mounted-volume discovery remains separate |
| Action popups | Desktop notifications D-Bus service | Completion status still appears in rummage |

The built-in icon font is compiled into the application; external themes are
optional. Application file dialogs require separate
[FileChooser backend selection](file-picker.md), not just a Wayland protocol.

`RUMMAGE_FORCE_CSD=1` requests the client-side title bar (the compositor decides
whether to honor it). `RUMMAGE_FORCE_XCURSOR=1` forces the cursor fallback. These
are useful for testing paths that a compositor would normally bypass.

## Reporting a compatibility problem

Include the compositor/version, GPU/driver, display scale, rummage package or
source revision, and the affected interaction. Identify whether the problem
occurs with the title bar, cursor, clipboard, drag and drop, application focus,
colour changes or file-picker integration. See [troubleshooting](troubleshooting.md)
for logs and configuration checks.

## Remote backends

Network discovery uses the separate `avahi-browse` executable and Avahi service.
File access uses gvfs (`gvfsd`, `gvfsd-fuse`, and the provider for SMB, SFTP, FTP,
WebDAV, NFS, MTP or gphoto2). No GLib library is loaded into the Rummage process.
See [remote locations and limits](usage.md#discovering-servers-and-attached-devices).
Encrypted volumes require UDisks2 plus a working polkit authorization agent when
policy requires authentication. Passphrases are not saved by Rummage.
