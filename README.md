# Seek

Search-first file explorer for Windows 11. One 0.5 MB exe, no installer, no runtime.

## Run
Double-click `Seek.exe`. Press **Ctrl+Shift+E** once for instant NTFS indexing (one UAC prompt; after that it starts without prompts).
To undo: `schtasks /delete /tn Seek /f`

## Search
`words`  `"exact phrase"`  `-exclude`  `ext:pdf,docx`  `in:projects`  `type:folder`
`size:>50mb`  `size:1mb..5mb`  `modified:today|week|3d|2024-05`  (size/date need a word, ext: or in: to narrow first)
Paths autocomplete: `C:\Users\`, `~\Desktop`, Tab to complete.

## Keys
Enter open · Ctrl+Enter show in folder · Esc clear/home · Backspace up · Alt+Left back · F2 rename · Del delete
Ctrl+C/X/V files · Ctrl+Shift+N new folder · Ctrl+P pin · Ctrl+H hidden files · Ctrl+L focus search

## Files it writes
`%LOCALAPPDATA%\Seek\` index.bin (cache), index.log (what the indexer decided), freq.txt (open history)
`%APPDATA%\Seek\pins.txt`

## Build
`build.bat` (MinGW-w64). To regenerate icons/logos from `logo_source\`: `python make_assets.py` (needs Pillow, numpy).

## Tests
`tests\selftest.inc` is a fake NTFS volume + change journal that drives the real admin-mode code
(compile with `-DSEEK_SELFTEST`, run under Wine). Scenarios: normal, `SEEK_FT_MODE=break`, `SEEK_FT_MODE=journalfail`.

## License
Seek is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE).
Commercial use requires permission from the copyright holder. This is source-available, not an OSI-approved open-source license.
