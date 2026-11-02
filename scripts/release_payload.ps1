# Shared production executables for portable, signing and symbol packaging.
$LulingReleaseExecutables = @('luling.exe', 'Luling.Index.exe', 'Luling.Document.exe',
    'Luling.Preview.exe', 'Luling.AI.exe', 'luling_shell.exe', 'luling_integration.exe', 'luling_elevated.exe')
$LulingReleaseRuntimeDlls = @('lumatext.dll', 'pdfium.dll')
$LulingReleasePayload = $LulingReleaseExecutables + $LulingReleaseRuntimeDlls
