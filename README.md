# LBM Animator

LBM Animator is a libretro core that displays and animates LBM (Interleaved
BitMap) image files with support for color-cycling animation and audio.

It's not particularly interactive. Its purpose is primarily as a screensaver;
I wrote it specifically to display images on an
[LED matrix display](https://github.com/0xe1f/red).

![Screenshot](doc/v04_lbm.png)  
![Screenshot](doc/v08am_lbm.png)  

## Features

- **LBM File Support**: Reads and parses IFF ILBM (Interleaved BitMap) format 
- **Audio File Support**: Reads and plays back 8SVX or OGG audio in background
- **Color Blending**: Smooth color transitions between animation frames

### Audio

The animator can play accompanying audio by loading `.ogg` files with the same
filename as the LBM image (e.g., `image.lbm` paired with `image.ogg`).

If the audio file is present in the `bios` directory, it will be played on loop.

## Building

```bash
make -f Makefile.libretro
```

## License

   Copyright 2026, Akop Karapetyan

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
