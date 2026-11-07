# Third-party notices

Q2Translate's own code (src/, data/services/, docs/, tools/, resources/) is released under the **MIT License** — see [LICENSE](LICENSE). Everything bundled from third parties keeps its own license; this file is the complete inventory.

The portable archive ships the license texts in `THIRD_PARTY_LICENSES/`.

## Linked and bundled components

| Component | License | Notice / text | Binaries in the package |
|---|---|---|---|
| **Qt 6.8.3** (Core, Gui, Widgets, Network, Qml, Concurrent, Svg, Xml) | LGPL-3.0 (alt. GPL-3.0 / commercial) | `third_party/qt/` | `Qt6*.dll`, `plugins/` |
| **Nuspell 5.1.9** | LGPL-3.0 | `third_party/nuspell/COPYING`, `COPYING.LESSER` | `libnuspell.dll` |
| **ICU 76** | ICU License | `third_party/icu/LICENSE` | `cygicuuc76.dll`, `cygicudata76.dll` |
| **PDFium** | BSD-3-Clause + sub-licenses (freetype, harfbuzz, libjpeg-turbo, libpng, openjpeg, zlib, …) | `third_party/pdfium/LICENSE`, `third_party/pdfium/licenses/` | `pdfium.dll` |
| **yaml-cpp** | MIT | `third_party/yaml-cpp/LICENSE` | built from source (`third_party/yaml-cpp/`), linked statically |
| **Tesseract 5 + Leptonica** (+ bundled dependency DLLs: libarchive, libpng, libjpeg, libtiff, openjp2, zlib, …) | Apache-2.0 | `third_party/tesseract/LICENSE` | `services/ocr/tesseract/tesseract.exe` and DLLs, separate process |
| **MinGW-w64 runtime** | GPL-3.0 with GCC Runtime Library Exception; winpthreads - permissive | `third_party/mingw-runtime/NOTICE.md` | `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` |
| **libwebp 1.4.0** (decoder only) | BSD-3-Clause | 	hird_party/libwebp/LICENSE | services/webp/libwebpdecoder.dll |
| **ONNX Runtime 1.20.1** | MIT | `third_party/onnxruntime/LICENSE` | `services/ocr/paddleocr/onnxruntime.dll` |
| **PP-OCRv5 models** (detection + English recognition) | Apache-2.0 | `services/ocr/paddleocr/models/v5/` | `.onnx` files loaded by ONNX Runtime |

## Optional external components (not shipped)

| Component | License | Used for | Where the user provides it |
|---|---|---|---|
| **DjVuLibre** (`ddjvu.exe`) | GPL-2.0-or-later | converting `.djvu` files to a rasterized PDF before OCR; invoked as a separate process, never linked | `services/ocr/djvu/ddjvu.exe` |

## Data files

| Data | License | Location |
|---|---|---|
| Hunspell-format spelling dictionaries, English (SCOWL) | per the `license` file shipped next to the dictionary | `services/spelling/hunspell/dictionaries/en/`, same for Nuspell |
| Hunspell-format spelling dictionary, Russian (A. I. Lebedev) | BSD-style, `license` file next to the dictionary | `services/spelling/hunspell/dictionaries/ru/` |
| Tesseract language data (`eng`, `rus`, `osd`) | Apache-2.0 | `services/ocr/tesseract/tessdata/` |
| XDXF dictionaries | varies per dictionary; **not bundled** — downloaded by the user | — |
| Connector icons | trademarks/favicons of the services, **not** covered by MIT | `data/services/ICONS.md` |

## LGPL compliance (Qt, Nuspell)

Qt and Nuspell are used under LGPL-3.0 and linked **dynamically**:

1. The LGPL-3.0 and GPL-3.0 texts are shipped in `THIRD_PARTY_LICENSES/` of the distributed archive.
2. Users can replace the Qt/Nuspell DLLs with another compatible build — relinking is possible without rebuilding the application.
3. The complete source code of Q2Translate is publicly available, satisfying the requirement to enable modification and relinking of the Combined Work.

This file is informational and does not replace the license texts referenced above.
