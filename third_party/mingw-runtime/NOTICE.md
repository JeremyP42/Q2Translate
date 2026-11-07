# MinGW runtime DLLs notice

The portable package ships these MinGW-w64 runtime libraries next to the
executable (deployed by `windeployqt --compiler-runtime`):

| File | License |
|---|---|
| `libgcc_s_seh-1.dll` | GPL-3.0 **with** GCC Runtime Library Exception |
| `libstdc++-6.dll` | GPL-3.0 **with** GCC Runtime Library Exception |
| `libwinpthread-1.dll` | MinGW-w64 runtime (permissive) |

The GCC Runtime Library Exception permits unrestricted use of these runtime
libraries with programs compiled by GCC, including closed-source and
MIT-licensed programs:

- Exception text: <https://www.gnu.org/licenses/gcc-exception-3.1.en.html>
- GCC runtime libraries source: included in every GCC release, <https://gcc.gnu.org/>
- MinGW-w64: <https://mingw-w64.org/>
