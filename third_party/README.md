# Third-party code

| What | Source | License |
|---|---|---|
| `tiny_dng_writer.h` | [syoyo/tinydngloader](https://github.com/syoyo/tinydngloader) @ ce55fd8, `examples/dngwriter` | MIT |
| `TinyMovFileLibrary.hpp` | [storyboardcreativity/tiny-mov-library](https://github.com/storyboardcreativity/tiny-mov-library) @ 03842f1 | MIT |
| `mpv/include/` | [mpv](https://github.com/mpv-player/mpv) client API headers | ISC |
| ReaLTaiizor 3.8.2.1 | [Taiizor/ReaLTaiizor](https://github.com/Taiizor/ReaLTaiizor), downloaded from NuGet by `build.bat` and embedded in the exe | MIT, see `ReaLTaiizor-LICENSE.txt` |

The two headers are unmodified and match the versions zraw-parser 0.11.0-alpha uses.

`zrawdng play` loads `libmpv-2.dll` at run time. To use it, put `libmpv-2.dll` from a
[shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake/releases) `mpv-dev-x86_64-v3`
package in `mpv\` and `build.bat` copies it next to the engine. ZRAW Studio doesn't need it.
