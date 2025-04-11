# vidpak

vidpak losslessly compresses 12-bit scientific video data at extremely high
speed while retaining good compression ratios.

## Installation

vidpak can be installed directly from Git using Python's pip (preferably with an
active virtual environment):

```
pip install git+https://github.com/tpwrules/vidpak
```

It should work on Linux, macOS, and Windows. Note that a compiler must be
installed to compile the Cython extensions; an error will occur if one is
unavailable.

This will make available the Python library for import using the name `vidpak`,
along with the command line tools.

## Command Line Usage

vidpak includes two command line tools, `vidpak` (for compression) and
`vidunpak` (for decompression). Each has a help available using e.g.
`vidpak -h`.

The raw video data format supported by these tools is 12-bit grayscale video,
with each pixel value encoded as a 2-byte native-endian (i.e. little) unsigned
integer from 0 to 4095. Pixel values are truncated to 12 bits; the high 4 bits
are ignored during compression and always zero after decompression.

Frames are stored with rows from top to bottom, then pixels from left to right
within each row. All frames must be the same size. If the input file is not an
integer number of frames, data in the last partial frame is ignored. There is
no header or padding for the file, nor each frame/row.

Specifying a frame size during compression is mandatory; specifying framerate is
possible but optional. Specifying a tile size during compression is also
possible; it is assumed to be the same as the frame size if not specified.

Such raw video can be played (to validate settings etc.) using a recent version
of FFmpeg (not included with vidpak) (optionally specifying the framerate):
```
ffplay -f rawvideo -pixel_format gray12le -video_size 1280x1024 [-framerate 60] -i data.raw
```

Basic compression (optionally specifying framerate and tile size):
```
vidpak -s 1280x1024 [-f 60] [-t 256x256] data.raw out.vidpak
```

Basic decompression:
```
vidunpak out.vidpak decompressed.raw
```

As the compression is lossless, the decompressed file will be identical to the
original (assuming the original has no out-of-range pixel values). However,
compression efficiency may be poorer (and some data at the end may be ignored)
if the frame size is not specified correctly.

## Library Usage

The command line tool implementation in `tools.py` serves as a reference example
for usage of the Python library. The Python library, contained in `file.py`,
also has docstrings. Both have good information on performant usage of the
library.

Basic compression sketch:
```
from vidpak import VidpakFileWriter
import numpy as np

frame = np.ones((64, 32), dtype=np.uint16)

writer = VidpakFileWriter("test.vidpak",
   size=frame.shape[::-1],
   bpp=12, # only supported value
)

for frame_num in range(10):
   frame[:, :5] = frame_num*100 # change some pixel values
   timestamp_us = int(frame_num*1e5)
   print(f"Frame {frame_num}'s ({timestamp_us}us) top-left value is {frame[0, 0]}")

   print(f"Writing frame {frame_num}...")
   writer.write_frame(timestamp_us, frame)

writer.close()
```

Basic decompression sketch:
```
from vidpak import VidpakFileReader

reader = VidpakFileReader("test.vidpak")
num_frames = reader.count_frames()

for frame_num in range(num_frames):
   timestamp_us, frame, _ = reader.read_frame(frame_num)

   print(f"Frame {frame_num}'s ({timestamp_us}us) top-left value is {frame[0, 0]}")

reader.close()
```


## License

This project is licensed under the Apache 2.0 license:

```
   vidpak
   Copyright 2021-2025 Thomas Watson

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

The Finite State Entropy library is originally licensed under the Simplified BSD
License, as described in `FiniteStateEntropy/LICENSE`.
