// epub_document.h — Quick Look reader for EPUB books.
//
// The spine documents become sections of the Markdown payload (doc_payload.h:
// one P record per chapter, then its blocks: headings, paragraphs with
// emphasis, lists, quotes, preformatted text, tables and pictures), the
// nav / NCX table of contents becomes C records and M carries the title and
// author. Scripts never run and nothing outside the package is fetched.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::preview {

bool IsEpubExtension(std::wstring_view extension);

// False when the file is not a readable EPUB (no container / package / spine).
bool MakeEpubDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                      bool& truncated);

}  // namespace pulse::preview
