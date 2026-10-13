// docx_document.h — Quick Look body text for Word documents (.docx/.docm).
//
// word/document.xml becomes the Markdown payload (doc_payload.h): headings
// (from paragraph styles / outline levels), paragraphs with bold, italic,
// underline, strike and web links, bullet and numbered lists, tables and
// inline pictures. Headers, footers, comments, footnotes and text boxes are
// left out. M record: "docx" and the word count.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::preview {

bool IsDocxExtension(std::wstring_view extension);

// False when the package has no readable main document (other decoders,
// e.g. the shell preview handler, get the file).
bool MakeDocxDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                      bool& truncated);

// Words as a reader counts them: every CJK character is one, other text by
// runs of letters and digits.
size_t CountWords(std::wstring_view text);

}  // namespace pulse::preview
