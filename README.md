# xemu with Groovy MiSTer NLC output

This xemu fork streams Xbox video and audio to a MiSTer running Groovy NLC. It can also use controllers attached to the MiSTer. For MiSTer installation, see [Groovy NLC releases](https://github.com/verbst/Groovy_MiSTer/releases).

Set up xemu's BIOS and disk images as described at [xemu.app](https://xemu.app).

Open **Settings > MiSTer** in xemu. 


## Required settings

| Setting | What to set |
| --- | --- |
| Stream to MiSTer | Turn on to start streaming. |
| MiSTer Address | Enter the MiSTer's IP address. Replace the default, `192.168.100.2`. |
| Monitor | Select the preset that matches the CRT connected to the MiSTer. The default is **Arcade 15kHz**. |

## Other settings

Defaults are shown in **bold**. Leave them as they are unless your setup needs a change.

### Connection

| Setting | Options and effect |
| --- | --- |
| Reconnect automatically | **On**. Rebuilds the stream after a connection loss. |

### Picture

| Setting | Options and effect |
| --- | --- |
| Mode Priority | **Keep Refresh Rate** preserves game speed. Keep Resolution may slow the game and affect audio. |
| Scan Mode | **Interlaced & Progressive** allows both and prefers higher resolution. Progressive Only avoids interlace resolution modes, eg: forcing 240p on a 15kHz CRT. |
| Aspect Ratio | **Follow Game**, Force 4:3, or Force 16:9. |
| Refresh Rate | **Automatic** follows the game's video mode. You can force NTSC (59.94 Hz), Exact 60 Hz, or PAL (50 Hz). |

### Stream

| Setting | Options and effect |
| --- | --- |
| Codec | **Near-lossless (NLC)** is the [default](https://github.com/verbst/Groovy_MiSTer#bandwidth). Uncompressed and LZ4 variants are also available. |
| Compression Pack | **Rice** or Tiled. Shown with NLC. |
| Detail | **1**. Shown with NLC. 0 = Lossless, 1, 2 and 3 reduce quality and [bandwidth](https://github.com/verbst/Groovy_MiSTer#bandwidth). |
| Colour Depth | **24-bit**, 32-bit, or 16-bit. Shown with non-NLC codecs. NLC uses 24-bit. |
| Packet Size | **Standard (1500)**. Jumbo (3800) requires jumbo frames on both the MiSTer and the computer's network adapter. |

### Timing

| Setting | Options and effect |
| --- | --- |
| Frame Pacing | **Follow the display** gives the game one frame per CRT frame. Independent clocks is a fallback. |

### Sound

| Setting | Options and effect |
| --- | --- |
| Send audio | **On**. Plays game audio through the MiSTer. |
| Buffer | **64 ms**. Shown when audio is on. Larger buffers reduce dropouts but add audio delay. Options: None, 16, 32, 64, or 128 ms. |
| This Computer's Speakers | **Silent**. Shown when audio is on. Also play here enables local sound; Off disables local output. |

### Controllers

| Setting | Options and effect |
| --- | --- |
| Use controllers attached to the MiSTer | **On**. Presents them to games as Xbox controllers. |
| Vibration | **On**. Shown when MiSTer controllers are enabled. GroovyNLC only. |

### This Window

| Setting | Options and effect |
| --- | --- |
| Keep showing the game here | **On**. Also shows the game in xemu's window. Menus remain available when it is off. |

### Diagnostics

| Setting | Options and effect |
| --- | --- |
| Logging | **Problems only**, Include timing, or Buffered trace. Stop streaming to write a buffered trace to the log. |

If xemu reports no response, check the MiSTer address, that the Groovy NLC core is running, and the network connection. The default network port is `32100`.
****
