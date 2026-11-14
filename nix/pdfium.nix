# Use the source recipe and hash-pinned sources from our locked nixpkgs revision.
# Keep its system libraries and upstream unit/embedder tests; do not fetch a
# third-party libpdfium.so. Updating flake.lock updates this recipe and its pins.
{ pdfium }:

pdfium.overrideAttrs (old: {
  # A custom file callback may refuse a read (our byte/time budget). PDFium 8110
  # CHECKs that read instead of propagating the error. Keep it a parse failure so
  # the helper can report exhaustion rather than killing the worker. Revisit this
  # small compatibility patch when updating the locked PDFium source revision.
  patches = old.patches ++ [ ./pdfium-read-error.patch ];
  # V8 and PartitionAlloc are already disabled by nixpkgs. State the XFA choice
  # explicitly too: the helper renders ordinary PDF/AcroForm appearances only.
  gnFlags = old.gnFlags ++ [ "pdf_enable_xfa=false" ];
  postInstall = (old.postInstall or "") + ''
    install -Dm644 ../../LICENSE "$out/share/licenses/pdfium/LICENSE"
  '';
})
