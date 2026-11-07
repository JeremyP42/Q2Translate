# Qt

QTranslate Remaster uses **Qt 6.8.3** (modules: Core, Gui, Widgets, Network, Qml, Concurrent, Svg, Xml) built with MinGW 13.1.

Qt is **not** part of this repository's MIT license. The application uses Qt under the **GNU LGPL-3.0**:

- Qt is linked dynamically (`Qt6*.dll` and plugins deployed next to the executable), so users can replace or update the Qt libraries independently.
- The full source code of QTranslate Remaster is available in the project repository, which satisfies the LGPL requirement to allow relinking with a modified version of the library.
- Qt source code: <https://download.qt.io/archive/qt/6.8/6.8.3/>

Files:

- `LICENSE.LGPL-3.0.txt` — GNU Lesser General Public License v3.0 (the license used).
- `LICENSE.GPL-3.0.txt` — GNU General Public License v3.0 (referenced by and shipped together with the LGPL-3.0).

Qt is alternatively available under a commercial license from The Qt Company.
