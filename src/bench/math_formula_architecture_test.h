#pragma once
#include <dwrite_2.h>
#include <string>
#include <vector>

namespace luling_test {
int TestMathArchitecture(IDWriteFactory2* factory, bool compatibility_only);
const std::wstring& MathArchitectureSource();
const std::vector<std::wstring>& MathArchitectureFormulas();
}
