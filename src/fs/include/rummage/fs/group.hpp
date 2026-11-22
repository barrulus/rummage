#pragma once
#include "rummage/fs/transfer.hpp"

namespace rummage::fs {
// Create destination exclusively, move the names, and retain its identity for undo.
// Undo moves them back and removes only the same, now-empty directory.
TransferResult run_group(const TransferRequest&, const core::CancelToken&,
                         const TransferCallbacks&);
}  // namespace rummage::fs
