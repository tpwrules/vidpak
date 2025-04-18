"""Read and write vidpak files."""

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
    """Read and unpack frames from a vidpak file.

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
            Path to the vidpak file on disk.
        endless: bool, optional, default False
            If True, the file is opened in endless mode.
        """
        self._opened = False
        # open the file and verify the header
        self._f = f = open(fname, "rb")
        header = f.read(32)
        if len(header) < 32:
            raise ValueError("truncated file header")
        if header[:6] != b'Vidpak':
            raise ValueError("not a vidpak file")
        version = struct.unpack("<H", header[6:8])[0]
        if version == 1:
            pass # packed data compatible with version 2
        elif version == 2:
            pass # current version
        else:
            raise ValueError(f"unknown file version {version}")
        self._file_version = version

        # read in the frame metadata and create the pack context
        width, height, bpp, twidth, theight, metadata_len = \
            struct.unpack("<IIIIII", header[8:])
        self.metadata = f.read(metadata_len)
        if len(self.metadata) < metadata_len:
            raise ValueError("truncated file header")
        self.size = (width, height)
        self.bpp = bpp
        self.tsize = (twidth, theight)
        self._ctx = _pack.PackContext(width, height, bpp, twidth, theight)

        # the file size and last index just count the frames that have been
        # accessed and verified to exist
        self.file_size = 32 + metadata_len
        self._last_header_index = -1 # index of the last header of the file

        self._frame_pos = None # if read from footer
        self._endless = endless
        self._frame_headers = {}
        self._have_all_headers = False
        self.frame_count = None

        if not self._endless:
            self._read_footer()

        self._rd_index = None
        self._rd_buf_curr = np.empty(
            (self._ctx.max_packed_size,), dtype=np.uint8)
        self._rd_buf_next = np.empty_like(self._rd_buf_curr)
        self._rd_chunks = None

        self._rd_cond = threading.Condition()
        self._rd_busy = False
        self._rd_exc = None

        self._opened = True
        self._rd_thread = threading.Thread(
            target=self._rd_thread_fn, daemon=True)
        self._rd_thread.start()

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
        prefetch : bool or int, default=True
            If True (the default), prefetch the frame at index+1 so that it is
            loaded from disk and ready for unpacking when read_frame(index+1) is
            called. If an int, prefetch that frame as described instead of
            index+1.

        Returns
        -------
        a tuple of
            the frame timestamp as an int in microseconds
            the numpy array the frame was unpacked into
            extra data as bytes
        """
        if not self._opened:
            raise ValueError("vidpak file is closed")

        index = int(index)
        if index < 0:
            raise ValueError("frame index must be non-negative")
        if not isinstance(prefetch, bool):
            prefetch = int(prefetch)
            if prefetch < 0:
                raise ValueError("prefetch index must be non-negative")
        elif prefetch is True:
            prefetch = index+1

        if self._rd_index != index: # are we reading the requested frame?
            with self._rd_cond: # nope, so start reading what the caller wants
                self._rd_wait() # wait for the previous read to complete
                self._rd_chunks = None # throw out the read data
                self._rd_index = index # and start it on the desired frame
                self._rd_busy = True
                self._rd_cond.notify()

        with self._rd_cond:
            self._rd_wait() # wait for the desired read to complete
            rd_chunks = self._rd_chunks # get the read data
            self._rd_chunks = None
            # if requested, start the worker reading the desired frame
            if prefetch is not False:
                self._rd_index = prefetch
                self._rd_busy = True
                self._rd_cond.notify()
            else:
                self._rd_index = None # no prefetch in progress

        if rd_chunks is None:
            raise IndexError("frame {} does not exist".format(index))

        header, packed_data, extra = rd_chunks
        frame_out = self._ctx.unpack(packed_data, frame_out)

        return header.timestamp, frame_out, extra

    def count_frames(self, max_counted=None):
        """Count and return the total number of frames in the file.

        This operation may require considerable time as the reader has to seek
        through the entire file to read every frame header (unless a valid
        footer was found).

        If max_counted is None, every remaining frame is counted. If not, at
        most max_counted frames will be counted. If there are still frames to be
        counted, None is returned, and the function can be called again to
        continue counting. Otherwise, the total number of frames is returned.

        In endless mode, at least the number of frames returned can be read.
        More frames may become available as the writer continues writing.
        """
        if max_counted is not None and max_counted < 0:
            raise ValueError(
                f"max_counted must be non-negative, not {max_counted}")

        if self.frame_count is not None and not self._endless:
            return self.frame_count

        with self._rd_cond:
            self._rd_wait() # wait for the worker so we can access the file

        if self._endless: self.frame_count = None
        if max_counted is None:
            while self.frame_count is None:
                # try to find an arbitrary future frame
                self._read_frame_header(len(self._frame_headers)+1000)
        elif max_counted > 0:
            self._read_frame_header(len(self._frame_headers)+max_counted-1)

        return self.frame_count

    def _read_frame_header(self, index):
        # read frame headers until the frame index `index` is found (or the file
        # ends). returns the header if it's found or None if not.
        try:
            return self._frame_headers[index] # we might have it already
        except KeyError:
            if self._have_all_headers: # no point looking harder
                return None

        def get(index, pos):
            # read, store, and return the index'th header at the given file pos.
            # returns None if unsuccessful
            self._f.seek(pos)

            header = self._f.read(16)
            if len(header) < 16: # header is incomplete; no more frames
                return None

            timestamp, data_size, extra_size = struct.unpack("<QII", header)
            if data_size == 0xFFFFFFFF and extra_size == 0xFFFFFFFF: # file end
                # writer has ended the file so endless mode is over
                self._endless = False
                return None
            data_pos = pos + 16
            file_size = data_pos + data_size + extra_size
            self._f.seek(file_size-1) # see if we can read the last byte of data
            if self._f.read(1) == b'':
                # we can't; this frame isn't complete and there are no more
                return None
            # update the end of the file
            self.file_size = max(file_size, self.file_size)
            self._last_header_index = max(index, self._last_header_index)

            header = FrameHeader(timestamp, data_size, extra_size, data_pos)
            self._frame_headers[index] = header

            return header

        if self._frame_pos is None: # if we don't already know the position
            # read more headers starting at the end of the file
            while (header := get(self._last_header_index+1, self.file_size)):
                if self._last_header_index == index:
                    return header
        else: # just read where we know it to be
            try:
                return get(index, int(self._frame_pos[index]))
            except IndexError:
                return None

        # the file is over and we did not find the header
        if not self._endless:
            self._have_all_headers = True # we have read all possible headers
            self.frame_count = len(self._frame_headers)
        return None

    def _rd_thread_fn(self):
        try:
            while True:
                with self._rd_cond:
                    # wait until we should be busy reading data
                    while not self._rd_busy: self._rd_cond.wait()
                    # if the file is closed, we don't have anything to do
                    if not self._opened: return
                    # read the data from the file
                    header = self._read_frame_header(self._rd_index)
                    if header is not None:
                        packed_data = self._rd_buf_curr[:header.data_size]
                        self._f.seek(header.data_pos)
                        self._f.readinto(packed_data)
                        extra = self._f.read(header.extra_size)
                        self._rd_chunks = [header, packed_data, extra]
                        # swap buffers so the next frame won't overwrite the
                        # buffer being read
                        self._rd_buf_next, self._rd_buf_curr = \
                            self._rd_buf_curr, self._rd_buf_next
                    else: # the requested frame didn't exist
                        self._rd_chunks = None
                    self._rd_busy = False # now we've finished our job
                    self._rd_cond.notify()
        except BaseException as e:
            with self._rd_cond:
                # store the exception for the main thread to re-raise
                self._rd_exc = e
                self._rd_busy = False # we're not doing anything any more
                self._rd_cond.notify()

    def _rd_wait(self, reraise=True):
        while self._rd_busy: self._rd_cond.wait()
        # if the worker thread crashed, close the vidpak file. the close
        # function will reraise the exception.
        if reraise and self._rd_exc is not None: self.close()

    def close(self):
        """Close the file."""
        if not self._opened: return
        # wait for the worker thread to finish what it's doing, then tell it to
        # stop by restarting it when the file is closed
        with self._rd_cond:
            self._rd_wait(reraise=False)
            self._opened = False
            self._rd_busy = True
            self._rd_cond.notify()
        self._rd_thread.join()
        self._f.close()
        if self._rd_exc is not None:
            rd_exc = self._rd_exc
            self._rd_exc = None
            raise RuntimeError("exception in reader thread") from rd_exc

    def _read_footer(self):
        if self._file_version == 1: # no footer in version 1 files
            return

        # validate the footer's footer at the last 16 bytes of the file
        self._f.seek(-16, 2)
        footer_info = self._f.read(16)
        if len(footer_info) != 16: return # incomplete
        if footer_info[:8] != b"VPFooter": return # invalid
        self._f.seek(struct.unpack("<Q", footer_info[8:])[0]) # seek to start
        if self._f.read(8) != b"VPFootSt": return # invalid footer start
        # we can't update the file size as that's used to control reading of 
        # frame headers

        # read the total frame count from the footer
        self.frame_count = struct.unpack("<I", self._f.read(4))[0]

        # read frame position table if it exists
        have_frame_pos = self._f.read(1)[0] != 0
        if have_frame_pos:
            self._frame_pos = \
                np.frombuffer(self._f.read(8*self.frame_count), dtype=np.uint64)

    def __del__(self):
        self.close()


class VidpakFileWriter:
    """Pack and write frames into a vidpak file.

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
            Path to the vidpak file on disk. It is created if it does not exist,
            or truncated if it does.
        size : (int, int)
            Tuple of the width and height of the frames to pack in pixels.
        bpp : int
            Bits per pixel of the frames to pack.
        tsize : (int, int), optional
            Tuple of the width and height of each packed tile. If None (the
            default), the frame size is used. The width and height of the tiles
            must be a multiple of the width and height of the frame.
        metadata : bytes-like, optional
            Metadata to write along with the file header. If None (the default),
            0 bytes are written and a 0-length bytes object will be read back.
        """
        self._opened = False
        # validate the metadata and create the pack context
        width, height, bpp = int(size[0]), int(size[1]), int(bpp)
        self.size = (width, height)
        self.bpp = bpp
        if tsize is None:
            twidth, theight = width, height
        else:
            twidth, theight = int(tsize[0]), int(tsize[1])
        self.tsize = (twidth, theight)
        self._ctx = _pack.PackContext(width, height, bpp, twidth, theight)
        
        if metadata is None:
            self.metadata = b''
        else:
            self.metadata = bytes(metadata)

        # open the file and write the header
        self._f = f = open(fname, "wb")
        f.write(b'Vidpak\x02\x00') # file version 2
        f.write(struct.pack("<IIIIII", # frame metadata
            width, height, bpp, twidth, theight, len(self.metadata)))
        f.write(self.metadata)
        f.flush() # ensure header is on disk for any readers
        self.file_size = 32 + len(self.metadata)
        self.frame_count = 0
        self._frame_pos = []

        self._wr_buf_curr = np.empty(
            (self._ctx.max_packed_size,), dtype=np.uint8)
        self._wr_buf_next = np.empty_like(self._wr_buf_curr)
        self._wr_chunks = None

        self._wr_cond = threading.Condition()
        self._wr_busy = False
        self._wr_exc = None

        self._opened = True
        self._wr_thread = threading.Thread(
            target=self._wr_thread_fn, daemon=True)
        self._wr_thread.start()

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
        if not self._opened:
            raise ValueError("vidpak file is closed")

        timestamp = int(timestamp)
        _, data_size = self._ctx.pack(frame, self._wr_buf_curr)
        if extra is None: extra = b''
        extra_size = len(extra)

        with self._wr_cond:
            self._wr_wait() # wait for any previous write to complete
            # store the byte position of this frame as the start of the header
            self._frame_pos.append(self.file_size)
            # store the data for the worker to write this frame
            self._wr_chunks = [
                struct.pack("<QII", timestamp, data_size, extra_size),
                self._wr_buf_curr[:data_size],
                extra,
            ]
            self.file_size += 16 + data_size + extra_size
            # and tell it to get back to work
            self._wr_busy = True
            self._wr_cond.notify()
        # swap buffers so next frame won't overwrite the buffer being written
        self._wr_buf_next, self._wr_buf_curr = \
            self._wr_buf_curr, self._wr_buf_next
        self.frame_count += 1

    def _wr_wait(self, reraise=True):
        while self._wr_busy: self._wr_cond.wait()
        # if the worker thread crashed, close the vidpak file. the close
        # function will reraise the exception.
        if reraise and self._wr_exc is not None: self.close()

    def _wr_thread_fn(self):
        try:
            while True:
                with self._wr_cond:
                    # wait until we should be busy writing data
                    while not self._wr_busy: self._wr_cond.wait()
                    # if the file is closed, we don't have anything more to do
                    if not self._opened: return
                    # write the data to the file
                    for chunk in self._wr_chunks:
                        self._f.write(chunk)
                    self._wr_chunks = None
                    self._f.flush() # flush the data so other readers can see it
                    self._wr_busy = False # now we've finished our job
                    self._wr_cond.notify()
        except BaseException as e:
            with self._wr_cond:
                # store the exception for the main thread to re-raise
                self._wr_exc = e
                self._wr_busy = False # we're not doing anything any more
                self._wr_cond.notify()

    def flush(self):
        """Flush the file from memory.

        Waits until the last frame has been completely written and the file has
        been flushed. This ensures the frame can be read by any open readers.
        Does not attempt to sync the file to disk.
        """
        if not self._opened:
            raise ValueError("vidpak file is closed")

        with self._wr_cond:
            self._wr_wait() # file is flushed by worker thread once it's done

    def close(self, write_frame_pos=True):
        """Close the file.

        This must be called before the writer is destroyed and the program exits
        to ensure all the data is fully written to the file. Otherwise, the last
        frame may be truncated and the footer will not be written.

        Parameters
        ----------
        write_frame_pos : bool
            If True (default), write a table of the byte positions of each frame
            for fast seeking to arbitrary frames.
        """
        if not self._opened: return
        # wait for the worker thread to finish what it's doing, then tell it to
        # stop by restarting it when the file is closed
        with self._wr_cond:
            self._wr_wait(reraise=False)
            self._opened = False
            self._wr_busy = True
            self._wr_cond.notify()
        self._wr_thread.join()
        if self._wr_exc is None:
            self._write_footer(write_frame_pos=write_frame_pos)
        self._f.close()
        if self._wr_exc is not None:
            wr_exc = self._wr_exc
            self._wr_exc = None
            raise RuntimeError("exception in writer thread") from wr_exc

    def _write_footer(self, write_frame_pos):
        # write a frame header with max sizes to signal end to endless readers
        self._f.write(struct.pack("<QII", 0, 0xFFFFFFFF, 0xFFFFFFFF))

        footer_pos = self._f.tell()
        # write the footer start magic
        self._f.write(b"VPFootSt")
        # write the number of frames in the file
        self._f.write(struct.pack("<I", self.frame_count))

        # write the frame positions, if asked
        write_frame_pos = int(bool(write_frame_pos))
        self._f.write(struct.pack("<b", write_frame_pos))
        if write_frame_pos:
            self._f.write(np.asarray(self._frame_pos, dtype=np.uint64))
        
        # write the actual footer magic and absolute footer start position.
        # these must be the last 16 bytes in the file
        self._f.write(b"VPFooter")
        self._f.write(struct.pack("<Q", footer_pos))

    def __del__(self):
        self.close()
