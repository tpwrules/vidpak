"""Pack and unpack frames in vidpak files.

vidpak losslessly compresses 12-bit scientific video data at extremely high
speed while retaining good compression ratios.

Each frame is first cut into a number of equally-sized non-overlapping tiles,
which are compressed independently. A prediction algorithm is run over each tile
to predict the next pixel in left to right, top to bottom order. The differences
between this prediction and the actual pixel values are then coded using the
Finite State Entropy algorithm.

Currently, only an averaging predictor over 12 bits per pixel data is supported.
"""

__version__ = "0.4.3"

from vidpak.file import VidpakFileReader, VidpakFileWriter

__all__ = ["VidpakFileReader", "VidpakFileWriter"]
