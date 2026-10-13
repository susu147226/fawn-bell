#pragma once
#include "search_query.h"

namespace pulse::app {
void RememberSearchQuery(AdvancedSearchSpec& spec, std::wstring_view raw);
std::wstring RewriteSearchQuery(const AdvancedSearchSpec& spec);
} // namespace pulse::app
