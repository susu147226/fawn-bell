// notebook_document.h — Quick Look for Jupyter notebooks (.ipynb).
//
// A notebook becomes a Markdown payload (markdown_document.h, PULSEMD) so the
// Markdown reader draws it: markdown cells render as Markdown, code cells and
// text outputs as code blocks with an In [n]: / Out[n]: gutter label (marker
// l<label>), PNG/JPEG/SVG outputs as local pictures (PreviewImageCache in
// doc_payload.h).
// Records added for notebooks: G (gutter levels) and I (kernel, cell count).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "../ipc/preview_protocol.h"

namespace pulse::preview {

bool IsNotebookExtension(std::wstring_view extension);

// False when the file is not a notebook (not JSON, no cells array), so other
// decoders get it.
bool MakeNotebookDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                          bool& truncated, ipc::PreviewTextEncoding& encoding);

}  // namespace pulse::preview
