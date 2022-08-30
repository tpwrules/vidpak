"""Read and write Vidpak files."""

import struct
import threading
from collections import namedtuple
import numpy as np

from vidpak import _pack

FrameHeader = namedtuple("FrameHeader", [
    "timestamp", # time, in microseconds, that this frame was captured
    "data_size", # size, in bytes, of the frame data
    "extra_size", # size, in bytes, of any extra data
    "data_pos", # absolute position, in bytes, of the data in the file
])


class VidpakFileReader:
    """Read and unpack frames from a Vidpak file.

    The reader is not thread-safe. It is possible to open the same file in
    different readers if necessary.

    Disk reads are performed in a worker thread for performance. By default,
    reading a frame will prefetch the next frame in the file.

    It is also possible to open a file for reading that is currently open for
    writing. Due to the asynchronous I/O, it is not guaranteed that all frames
    written will immediately be available for reading, even if the reader and
    writer are in the same thread.

    In endless mode, the reader will not assume the number of frames is fixed
    and will always try to read more data from the file if the requested frame
    is not available. This is required when the file is also open for writing.

    Attributes
    ----------
    size : (int, int)
        Tuple of the width and height of each frame in pixels.
    bpp : int
        The number of bits per pixel of each frame.
    tsize : (int, int)
        Tuple of the width and height of each packed tile in pixels.
    file_size : int
        Current size of the input file, in bytes. May be smaller than the number
        of bytes actually in the file if its frames have not been completely
        counted, or due to asynchronous reads or a truncated frame.
    frame_count : int
        Number of frames in the input file, or None if not known. Call
        count_frames to update (and return) this value.
    metadata : bytes
        The metadata that was written along with the file header.
    """
    def __init__(self, fname, endless=False):
        """
        Parameters
        ----------
        fname : str or pathlib.Path object
            Path to the Vidpak file on disk.
        endless: bool, optional, default False
            If True, the file is opened in endless mode.
        """
        self.opened = False
        # open the file and verify the header
        self.f = f = open(fname, "rb")
        header = f.read(8)
        if header[:6] != b'Vidpak':
            raise ValueError("not a vidpak file")
        version = struct.unpack("<H", header[6:])[0]
        if version != 1:
            raise ValueError("unknown file version {}".format(version))

        # read in the frame metadata and create the pack context
        width, height, bpp, twidth, theight, metadata_len = \
            struct.unpack("<IIIIII", f.read(24))
        self.metadata = f.read(metadata_len)
        self.size = (width, height)
        self.bpp = bpp
        self.tsize = (twidth, theight)
        self.ctx = _pack.PackContext(width, height, bpp, twidth, theight)

        self.file_size = 32 + metadata_len
        self.endless = endless
        self.frame_headers = []
        self.frame_count = None

        self.rd_index = None
        self.rd_buf_curr = np.empty((self.ctx.max_packed_size,), dtype=np.uint8)
        self.rd_buf_next = np.empty_like(self.rd_buf_curr)
        self.rd_chunks = None

        self.rd_cond = threading.Condition()
        self.rd_busy = False
        self.rd_exc = None

        self.opened = True
        rd_thread = threading.Thread(target=self._rd_thread_fn, daemon=True)
        rd_thread.start()

    def read_frame(self, index, frame_out=None, prefetch=True):
        """Read and unpack the given frame from the file.

        Raises IndexError if the requested frame does not exist in the file.

        Parameters
        ----------
        index : int
            The 0-based index of the requested frame.
        frame_out : numpy array, optional
            The array to unpack the frame into. If None (the default), a new
            array is created and returned. Otherwise the same array is returned.
        prefetch : bool, optional
            If True (the default), prefetch the frame at index+1 so that it is
            loaded from disk and ready for unpacking when read_frame(index+1) is
            called.

        Returns
        -------
        a tuple of
            the frame timestamp as an int in microseconds
            the numpy array the frame was unpacked into
            extra data as bytes
        """
        if not self.opened:
            raise ValueError("vidpak file is closed")

        index = int(index)
        if index < 0:
            raise ValueError("frame index must be non-negative")

        if self.rd_index != index: # are we reading the frame the caller wanted?
            with self.rd_cond: # nope, so start reading what they do want
                # wait until the worker thread is done reading the previous data
                while self.rd_busy: self.rd_cond.wait()
                # if the worker thread crashed, close the vidpak file. the close
                # function will reraise the exception.
                if self.rd_exc is not None: self.close()
                self.rd_chunks = None # throw out the read data
                self.rd_index = index # and start it on the desired frame
                self.rd_busy = True
                self.rd_cond.notify()

        with self.rd_cond:
            # wait for the worker thread to finish reading the desired frame
            while self.rd_busy: self.rd_cond.wait()
            if self.rd_exc is not None: self.close()
            rd_chunks = self.rd_chunks # get the read data
            self.rd_chunks = None
            # if requested, start it reading the next frame
            if prefetch:
                self.rd_index = index + 1
                self.rd_busy = True
                self.rd_cond.notify()

        if rd_chunks is None:
            raise IndexError("frame {} does not exist".format(index))

        header, packed_data, extra = rd_chunks
        frame_out = self.ctx.unpack(packed_data, frame_out)

        return header.timestamp, frame_out, extra

    def count_frames(self, max_counted=None):
        """Count and return the total number of frames in the file.

        This operation may require considerable time as the reader has to seek
        through the entire file to read every frame header.

        If max_counted is None, every remaining frame is counted. If not, at
        most max_counted frames will be counted. If there are still frames to be
        counted, None is returned, and the function can be called again to
        continue counting. Otherwise, the total number of frames is returned.

        In endless mode, at least the number of frames returned can be read.
        More frames may become available as the writer continues writing.
        """
        if max_counted is not None and max_counted <= 0:
            raise ValueError(
                "max counted {} must be positive".format(max_counted))

        if self.frame_count is not None and not self.endless:
            return self.frame_count

        with self.rd_cond:
            # wait until the worker thread is done so we can access the file
            while self.rd_busy: self.rd_cond.wait()
            if self.rd_exc is not None: self.close()

        if self.endless: self.frame_count = None
        if max_counted is None:
            while self.frame_count is None:
                # try to find an arbitrary future frame
                self._read_frame_header(len(self.frame_headers)+1000)
        else:
            self._read_frame_header(len(self.frame_headers)+max_counted)

        return self.frame_count

    def _read_frame_header(self, index):
        # read frame headers until the frame index `index` is found (or the file
        # ends). returns the header if it's found or None if not.
        if len(self.frame_headers) > index:
            return self.frame_headers[index] # we have it already

        if self.frame_count is not None: # we've reached the end of the file
            return None

        self.f.seek(self.file_size)
        while len(self.frame_headers) <= index:
            header = self.f.read(16)
            if len(header) < 16: # header is incomplete; no more frames
                break

            timestamp, data_size, extra_size = struct.unpack("<QII", header)
            data_pos = self.file_size + 16
            file_size = data_pos + data_size + extra_size
            self.f.seek(file_size-1) # see if we can read the last byte of data
            if self.f.read(1) == b'':
                # we can't; this frame isn't complete and there are no more
                break
            self.file_size = file_size

            self.frame_headers.append(
                FrameHeader(timestamp, data_size, extra_size, data_pos))
        else: # the loop did not break which means we found the requested header
            return self.frame_headers[index]

        # the file is over and we did not find the header
        if not self.endless:
            self.frame_count = len(self.frame_headers)
        return None

    def _rd_thread_fn(self):
        try:
            while True:
                with self.rd_cond:
                    # wait until we should be busy reading data
                    while not self.rd_busy: self.rd_cond.wait()
                    # if the file is closed, we don't have anything to do
                    if not self.opened: return
                    # read the data from the file
                    header = self._read_frame_header(self.rd_index)
                    if header is not None:
                        packed_data = self.rd_buf_curr[:header.data_size]
                        self.f.seek(header.data_pos)
                        self.f.readinto(packed_data)
                        extra = self.f.read(header.extra_size)
                        self.rd_chunks = [header, packed_data, extra]
                        # swap buffers so the next frame won't overwrite the
                        # buffer being read
                        self.rd_buf_next, self.rd_buf_curr = \
                            self.rd_buf_curr, self.rd_buf_next
                    else: # the requested frame didn't exist
                        self.rd_chunks = None
                    self.rd_busy = False # now we've finished our job
                    self.rd_cond.notify()
        except Exception as e:
            with self.rd_cond:
                # store the exception for the main thread to re-raise
                self.rd_exc = e
                self.rd_busy = False # we're not doing anything any more
                self.rd_cond.notify()

    def close(self):
        """Close the file."""
        if not self.opened: return
        # wait for the worker thread to finish what it's doing, then tell it to
        # stop by restarting it when the file is closed
        with self.rd_cond:
            while self.rd_busy: self.rd_cond.wait()
            self.opened = False
            self.rd_busy = True
            self.rd_cond.notify()
        self.f.close()
        if self.rd_exc is not None:
            rd_exc = self.rd_exc
            self.rd_exc = None
            raise RuntimeError("exception in reader thread") from rd_exc

    def __del__(self):
        self.close()


class VidpakFileWriter:
    """Pack and write frames into a Vidpak file.

    The writer is not thread-safe. It is possible to open the file in one or
    more readers. See the documentation on VidpakFileReader for caveats.

    Disk writes are performed in a worker thread for performance.

    Attributes
    ----------
    size : (int, int)
        Tuple of the width and height of each frame in pixels.
    bpp : int
        The number of bits per pixel of each frame.
    tsize : (int, int)
        Tuple of the width and height of each packed tile in pixels.
    file_size : int
        Current size of the output file, in bytes. May be larger than the number
        of bytes actually in the file due to asynchronous writes.
    frame_count : int
        Number of frames that have been written to the output file. This is
        exactly the number of times write_frame has been called.
    metadata : bytes
        The metadata that was written along with the file header.
    """
    def __init__(self, fname, size, bpp, tsize=None, metadata=None):
        """
        Parameters
        ----------
        fname : str or pathlib.Path object
            Path to the Vidpak file on disk. It is created if it does not exist,
            or truncated if it does.
        size : (int, int)
            Tuple of the width and height of the frames to pack in pixels.
        bpp : int
            Bits per pixel of the frames to pack.
        tsize : (int, int), optional
            Tuple of the width and height of each packed tile. If None (the
            default), the frame size is used. The width and height of the tiles
            must be a multiple of the width and height of the frame, and the
            height of the tiles must be a multiple of 4.
        metadata : bytes-like, optional
            Metadata to write along with the file header. If None (the default),
            0 bytes are written and a 0-length bytes object will be read back.
        """
        self.opened = False
        # validate the metadata and create the pack context
        width, height, bpp = int(size[0]), int(size[1]), int(bpp)
        self.size = (width, height)
        self.bpp = bpp
        if tsize is None:
            twidth, theight = width, height
        else:
            twidth, theight = int(tsize[0]), int(tsize[1])
        self.tsize = (twidth, theight)
        self.ctx = _pack.PackContext(width, height, bpp, twidth, theight)
        
        if metadata is None:
            self.metadata = b''
        else:
            self.metadata = bytes(metadata)

        # open the file and write the header
        self.f = f = open(fname, "wb")
        f.write(b'Vidpak\x01\x00') # file version 1
        f.write(struct.pack("<IIIIII", # frame metadata
            width, height, bpp, twidth, theight, len(self.metadata)))
        f.write(self.metadata)
        self.f.flush() # ensure header is on disk for any readers
        self.file_size = 32 + len(self.metadata)
        self.frame_count = 0

        self.wr_buf_curr = np.empty((self.ctx.max_packed_size,), dtype=np.uint8)
        self.wr_buf_next = np.empty_like(self.wr_buf_curr)
        self.wr_chunks = None

        self.wr_cond = threading.Condition()
        self.wr_busy = False
        self.wr_exc = None

        self.opened = True
        wr_thread = threading.Thread(target=self._wr_thread_fn, daemon=True)
        wr_thread.start()

    def write_frame(self, timestamp, frame, extra=None):
        """Pack and write the given frame to the file.

        Parameters
        ----------
        timestamp : int
            The timestamp of the frame as the number of microseconds from the
            start of the recording.
        frame : numpy array
            The array containing the frame to pack.
        extra : bytes-like, optional
            Extra data to write along with the frame. If None (the default), 0
            bytes are written and a 0-length bytes object will be read back.
        """
        if not self.opened:
            raise ValueError("vidpak file is closed")

        timestamp = int(timestamp)
        _, data_size = self.ctx.pack(frame, self.wr_buf_curr)
        if extra is None: extra = b''
        extra_size = len(extra)

        with self.wr_cond:
            # wait until the worker thread is done writing the previous data
            while self.wr_busy: self.wr_cond.wait()
            # if it crashed, close the vidpak file. the close function will
            # reraise the exception.
            if self.wr_exc is not None: self.close()
            # store the data for it to write this frame
            self.wr_chunks = [
                struct.pack("<QII", timestamp, data_size, extra_size),
                self.wr_buf_curr[:data_size],
                extra,
            ]
            self.file_size += 16 + data_size + extra_size
            # and tell it to get back to work
            self.wr_busy = True
            self.wr_cond.notify()
        # swap buffers so the next frame won't overwrite the buffer being written
        self.wr_buf_next, self.wr_buf_curr = self.wr_buf_curr, self.wr_buf_next
        self.frame_count += 1

    def _wr_thread_fn(self):
        try:
            while True:
                with self.wr_cond:
                    # wait until we should be busy writing data
                    while not self.wr_busy: self.wr_cond.wait()
                    # if the file is closed, we don't have anything more to do
                    if not self.opened: return
                    # write the data to the file
                    for chunk in self.wr_chunks:
                        self.f.write(chunk)
                    self.wr_chunks = None
                    self.f.flush() # flush the data so other readers can see it
                    self.wr_busy = False # now we've finished our job
                    self.wr_cond.notify()
        except Exception as e:
            with self.wr_cond:
                # store the exception for the main thread to re-raise
                self.wr_exc = e
                self.wr_busy = False # we're not doing anything any more
                self.wr_cond.notify()

    def flush(self):
        """Flush the file from memory.

        Waits until the last frame has been completely written and the file has
        been flushed. This ensures the frame can be read by any open readers.
        Does not attempt to sync the file to disk.
        """
        if not self.opened:
            raise ValueError("vidpak file is closed")

        with self.wr_cond:
            # wait until the worker thread is done
            while self.wr_busy: self.wr_cond.wait()
            if self.wr_exc is not None: self.close()
            # the file has now been flushed by the worker thread

    def close(self):
        """Close the file.

        This must be called before the writer is destroyed and the program exits
        to ensure all the data is fully written to the file. Otherwise, the last
        frame may be truncated.
        """
        if not self.opened: return
        # wait for the worker thread to finish what it's doing, then tell it to
        # stop by restarting it when the file is closed
        with self.wr_cond:
            while self.wr_busy: self.wr_cond.wait()
            self.opened = False
            self.wr_busy = True
            self.wr_cond.notify()
        self.f.close()
        if self.wr_exc is not None:
            wr_exc = self.wr_exc
            self.wr_exc = None
            raise RuntimeError("exception in writer thread") from wr_exc

    def __del__(self):
        self.close()
