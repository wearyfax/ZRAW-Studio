# ZRAW Studio

<img src="docs/icon.png" alt="ZRAW Studio icon" width="96">

![ZRAW Studio converting a folder of ZRAW clips](docs/screenshot.webp)

ZRAW Studio converts Z CAM ZRAW footage into files DaVinci Resolve and other editors can open, and lets Windows show and play ZRAW clips. Drop your clips on the window and press Start.

- Converts 4K ZRAW to CinemaDNG at up to 130 frames per second on an RTX 3070.
- Makes lossy files that look the same as lossless, at a quarter of the uncompressed size. The default 3:1 CinemaDNG takes about 4 MB per 4K frame, against 6.9 MB for lossless and 16.6 MB uncompressed, and its error is about a sixth of the noise the sensor already records. Lossless and uncompressed are there when you want them.
- Removes noise from the raw Bayer data on the GPU, before the DNGs are written. Each frame is merged with its neighbors using the camera's measured noise at the clip's ISO, and anything that moves keeps its own pixels, so nothing ghosts. Light mode takes out more than half the noise without softening edges. Denoise works best on tripod and gimbal shots.
- Works by drag and drop. Drop in clips, folders or a whole memory card, check the thumbnails, double-click one to preview it, and press Start. The defaults are ready for Resolve.
- Shows ZRAW clips in File Explorer with real thumbnails, icons and the preview pane, after one click on the Tagger tab.
- Plays ZRAW at full 4K with frame-accurate seeking in [mpv](https://mpv.io/installation/), a free video player, once you add ZRAW Studio's plugin on the Player tab.
- Uses your graphics card for compression, and can split the work across several. Without a suitable GPU it runs on the CPU.
- Renames ZRAW clips. The camera gives ZRAW and H.265 recordings the same `.MOV` name, so the Tagger tab finds the ZRAW clips by their contents and adds a `ZRAW_` prefix. It can undo that too.
- Handles both kinds of ZRAW. Original Bayer ZRAW (firmware before 0.94, or patched firmware) becomes CinemaDNG. H.265 ZRAW (firmware 0.94 and later) becomes a standard H.265 `.mov` in seconds, with no re-encoding.
- Gets color right. The default color matrices come from spectrophotometer measurements of an E2 and follow each shot's color temperature, and Resolve's Highlight Recovery works on the converted files.
- Runs as a single portable `.exe` with nothing to install.

> [!NOTE]
> The conversion settings (compression, color, denoise and resolution) only apply to Bayer ZRAW. H.265 ZRAW is already processed and compressed by the camera, so ZRAW Studio copies its video into a standard `.mov` unchanged.

## Download

Get `ZRAW Studio.exe` from the [latest release](https://github.com/wearyfax/ZRAW-Studio/releases/latest) and run it. The first time, Windows may say it protected your PC because the app is new and unsigned. Click **More info**, then **Run anyway**.

You need:

- Windows 10 or 11, 64-bit
- A processor with AVX2: Intel Core 4th generation or newer, or AMD Ryzen or newer
- For GPU acceleration: a DirectX 11 graphics card
- For H.265 ZRAW thumbnails and previews: Microsoft's [HEVC Video Extensions](https://apps.microsoft.com/detail/9nmzlz57r3t7)
- To play ZRAW clips: [mpv](https://mpv.io/installation/), a free video player. The ZRAW plugin runs inside mpv, so install mpv first.

## Using it

1. Drop your clips, or the card's `DCIM` folder, anywhere on the Convert tab.
2. Check the folder on the right. By default the files go to `zraw-decoded` at the root of the drive the clips are on.
3. Press **Start**.

Each Bayer clip becomes a folder of DNG files plus a `.mov` with the audio and timecode. Drag the folder into DaVinci Resolve.

To see ZRAW thumbnails in File Explorer, open the **Tagger** tab and click **Install thumbnail engine**.

Playing ZRAW clips needs [mpv](https://mpv.io/installation/). ZRAW Studio's plugin teaches mpv to read ZRAW, so install mpv first, then open the **Player** tab and click **Install plugin**. After that, mpv plays ZRAW clips like any other video.

The **About** tab shows your version, checks for updates and opens the log folder. If something goes wrong, attach the newest log file to an [issue](https://github.com/wearyfax/ZRAW-Studio/issues).

### Fixed ratio or fixed quality

The Compression menu on the Convert tab has two modes:

- **Fixed ratio** (the default) compresses every frame by the same amount. At 3:1 the result looks the same as lossless. For the highest quality, use fixed ratio.
- **Fixed quality** sizes each shot from its ISO and content to reach a chosen level, named after the ProRes or E2 H.265 setting it matches. The files come out about the same size on average, but at the higher compression levels some shots can show compression artifacts.

## License

ZRAW Studio is free software under the [GNU General Public License v2](LICENSE). Bundled third-party code is listed in [third_party/README.md](third_party/README.md).

ZRAW Studio is an independent project and is not affiliated with or endorsed by Z CAM.

## Building from source

You need Windows 10 or 11, [Git](https://git-scm.com/), and the free [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) with the **Desktop development with C++** workload. The C# part compiles with the .NET Framework compiler built into Windows.

```bat
git clone https://github.com/wearyfax/ZRAW-Studio.git
cd ZRAW-Studio
build.bat
```

The first build downloads the ReaLTaiizor UI library from NuGet. The result is `bin\ZRAW Studio.exe`. The build also leaves the command-line converter, `bin\zrawdng.exe`, in the same folder; run it without arguments to see its options.

| Folder | Contents |
|---|---|
| `app\` | The Windows app (C#) |
| `engine\` | The converter, the preview library and the mpv plugin (C++) |
| `explorer\` | The File Explorer add-on for thumbnails and previews (C++) |
| `third_party\` | Libraries from other projects |

## Thanks

ZRAW Studio is built on the work of [storyboardcreativity](https://github.com/storyboardcreativity), author of [zraw-parser](https://github.com/storyboardcreativity/zraw-parser), [zraw-decoder-lib](https://github.com/storyboardcreativity/zraw-decoder-lib) and [tiny-mov-library](https://github.com/storyboardcreativity/tiny-mov-library). The ZRAW decoder here is a port of zraw-decoder-lib, and the converted folders use zraw-parser's layout. Thank you.
