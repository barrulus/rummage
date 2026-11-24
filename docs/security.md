# Security model

[Documentation](README.md) · [Architecture](../contributing/architecture.md) · [File operations](file-operations.md)

rummage separates image, text and archive file parsing from the browser process and uses bounded
protocols and resource limits. This page describes those boundaries, not a claim
that the entire file manager or every external program is sandboxed.

## File-reading helpers

`rummage-preview` receives already-open input descriptors from rummage and returns
picture/text results or archive metadata and member bytes. Libraries are warmed up before entering the sandbox so later
decoding does not need to discover configuration or load codec resources from disk.

In normal builds the helper applies:

- A seccomp syscall allowlist. File opens fail with `EACCES`; unexpected calls
  normally kill the helper. The helper exits if seccomp setup fails.
- Landlock restrictions when supported by the running kernel. Filesystem access
  is denied; network and cross-process restrictions depend on the available ABI.
  Landlock is additional protection, not a requirement for the helper to start.
- Resource limits: no core dumps, 32 file descriptors and 4 GiB of virtual address
  space in ordinary builds. Decoder and protocol limits also bound requests/results.
- Parent-side timeout and failure handling. The default helper request timeout is
  3 seconds (8 seconds for media and PDF; archive extraction can request up to
  60 seconds), and idle helpers close after 30 seconds; allocations can be returned
  earlier during idle time.

The filter has architecture definitions for x86-64 and AArch64. Crashes, timeouts
and malformed replies are treated as preview failures. Returned pixel dimensions
and overflow-checked buffer sizes are validated before pixels enter the application.
The browser does not fall back to decoding the same unsupported input in-process.

SVG file previews run in the base helper using librsvg's default parsing limits,
the helper's process memory ceiling and request deadline. The adapter reads at most 8 MiB and
caps the rendered side at 1024 pixels. It reads only the supplied descriptor;
external SVG resources are never fetched. Theme icons use the same adapter in-process,
without the helper's memory ceiling or request deadline. Preview SVG text uses a
private font map prepared before sandboxing:
at most 12 generic font files, 64 faces and 64 MiB of FreeType font data (plus
HarfBuzz's font mappings). Seccomp is synchronized across existing helper threads,
including Pango's font matcher; it cannot open paths or create network sockets.
Untrusted `.directory` files may select theme icon names only; custom image paths
are ignored.

Markdown parsing runs in the text helper. The browser validates a bounded presentation
document before laying it out; HTML is not executed, and links and images do not trigger
resource loads. Source and rendered previews retain the text reader's byte limits.

Archive browsing lists ZIP, TAR and gzip data through the base helper. Member previews
are decoded from memory descriptors. Listing sizes, path depth and synthesized folder
counts are bounded before the browser constructs its tree. Archive contents are read-only.

## Parsing and execution boundaries

| Component | Boundary |
| --- | --- |
| Theme SVG/PNG icons | Loaded/rendered in-process; SVG uses librsvg defaults with 8 MiB input and 1024-pixel output-side caps, without a process memory ceiling or deadline |
| Thumbnail cache | Shared desktop files read by the application; not a trusted private store |
| Optional video previews | `rummage-media` links FFmpeg libraries inside the same descriptor-based sandbox, with separate decode/time limits |
| Optional PDF previews | `rummage-pdf` links PDFium inside the same sandbox: PDF parser only, no JavaScript engine, no system fonts, 4 GiB address-space limit, bounded reads, drawing stopped at a deadline |
| Desktop applications and custom actions | Ordinary user processes, outside the preview sandbox |
| Archive member extraction | Archive decoding in the helper; rummage creates destination entries through confined directory descriptors |
| External archive extraction | External archive tools; their parser and extraction behavior remains their responsibility |
| Archive creation | A worker reads selected local files and writes ZIP/tar.gz; it is outside the preview sandbox |
| File operations, MIME lookup and desktop metadata | Run with rummage's user permissions |

Do not infer the helper's protections for any other row. See
[SVG support and limits](previews.md#svg) for the supported preview subset.

## File-operation guarantees and limits

Built-in copies stage output before publishing it and cross-filesystem moves flush
the copy before removing the source. Conflicts ask for a policy, trash failures do
not become silent permanent deletes, and permanent deletion asks for confirmation.
These measures do not make all multi-file operations transactional or replace backups.
Completed steps can remain after cancellation, and overwritten or permanently
deleted content is not generally recoverable through Undo.

Batch rename stages all changed names in a private directory and uses atomic no-overwrite
renames, including for swaps and rollback. It rechecks source identities and refuses file
systems without the required operation. A failure rolls back completed steps; if concurrent
changes prevent recovery, the error details identify the retained files. Undo/Redo uses the
same checks and treats the batch as one operation.

Copying members out of a browsed archive creates files in a private staging directory,
using `openat2` confinement to prevent traversal and symlink escapes. Publication never
replaces an existing name. Special files and unsupported/encrypted members are refused or
reported; absolute symlink targets and targets containing any `..` component are
rejected, including otherwise harmless parent-relative links. These guarantees
apply to the built-in member workflow, not to independently configured external tools.

Custom actions use an argument vector, preserving path boundaries without implicit
shell evaluation, globbing or environment expansion. A user-configured shell or
executable still has the user's normal authority. The destination picker confirms
an existing save path but the external program controls actual writes and conflicts.
External changes are outside rummage's undo history.

Archive CLI workflows use new destination names and preserve sources, but are not
the image/text decoder sandbox. Consult the external tool's behavior for untrusted
archives. This documentation does not claim a universal archive containment guarantee.

## Diagnostic builds and settings

Sanitizer builds relax address-space limits and add syscall allowances for runtime
instrumentation. They are useful for finding errors but are not equivalent to the
production sandbox. `RUMMAGE_SANDBOX_REPORT=1` logs unexpected syscalls and returns
`ENOSYS` instead of using the normal kill path; use it for diagnosis, not as a
normal runtime setting.

When investigating a failure, retain the input, revision, build mode, stderr and
reproduction command. Avoid putting private file contents into a public report.
For review and test workflows, see [development](../contributing/development.md).
