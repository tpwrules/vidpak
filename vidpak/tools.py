import sys
import time
import pathlib
import argparse
import threading
import queue
import numpy as np

from vidpak import VidpakFileReader, VidpakFileWriter

def main_pack():
    parser = argparse.ArgumentParser(
        description="Pack raw video data into a Vidpak file.")
    parser.add_argument('input', type=str,
        help="Path to raw input file (or - to read from stdin).")
    parser.add_argument('output', type=str,
        help="Path to Vidpak output file.")
    parser.add_argument('-s', '--size', type=str, required=True,
        help="Width and height (as in WxH) of each frame.")
    parser.add_argument('-t', '--tile-size', type=str,
        help="Width and height (as in WxH) of each packed tile.")
    parser.add_argument('-n', '--num-frames', type=int,
        help="Only pack the first n frames.")
    parser.add_argument('-f', '--framerate', type=float, default=30,
        help="Nominal framerate used for determining frame timestamps.")
    parser.add_argument('--no-frame-pos', action="store_true",
        help="Don't write frame position table.")
    parser.add_argument('--verify', action="store_true",
        help="Unpack each frame and verify it matches the original.")

    args = parser.parse_args()

    size = tuple(int(d) for d in args.size.split("x"))
    if args.tile_size is not None:
        tile_size = tuple(int(d) for d in args.tile_size.split("x"))
    else:
        tile_size = size

    if len(size) != 2:
        raise ValueError(f"size {size} must be exactly 2 dimensions")
    if len(tile_size) != 2:
        raise ValueError(f"tile size {tile_size} must be exactly 2 dimensions")
    if args.framerate <= 0:
        raise ValueError(f"framerate {args.framerate} must be positive")
    if args.num_frames is not None and args.num_frames <= 0:
        raise ValueError(f"number of frames {args.num_frames} must be positive")

    if args.input == "-":
        fin = sys.stdin.buffer
    else:
        fin = open(args.input, "rb")
    # for now we can only deal with 12bpp files
    writer = VidpakFileWriter(args.output, size, 12, tile_size)
    if args.verify:
        reader = VidpakFileReader(args.output, endless=True)
    else:
        reader = None

    empty_frames, full_frames = queue.Queue(), queue.Queue()
    verify_frames = queue.Queue()
    for _ in range(4):
        empty_frames.put(np.empty((size[1], size[0]), dtype=np.uint16))
    frame_size = size[0]*size[1]*2

    def read_thread_fn():
        while True:
            frame = empty_frames.get()
            # if we didn't read a complete frame, we're done reading
            if fin.readinto(frame) != frame_size: break
            full_frames.put(frame)
        fin.close()
        full_frames.put(None)

    verify_result = True
    def verify_thread_fn():
        nonlocal verify_result
        got_frame = np.empty((size[1], size[0]), dtype=np.uint16)
        index = 0
        while True:
            frame = verify_frames.get()
            if frame is None: break # no more frames
            if reader is not None and verify_result is True:
                while True:
                    try:
                        reader.read_frame(index, frame_out=got_frame)
                        break
                    except IndexError: # frame is not ready yet
                        pass
                if not np.array_equal(frame, got_frame):
                    verify_result = False
                index += 1
            empty_frames.put(frame)

    num_frames = 0
    pack_time = 0
    read_thread = threading.Thread(target=read_thread_fn, daemon=True)
    read_thread.start()
    verify_thread = threading.Thread(target=verify_thread_fn, daemon=True)
    verify_thread.start()
    while True:
        frame = full_frames.get()
        if frame is None: break

        # time how long packing the frame takes
        s = time.perf_counter()
        writer.write_frame(int((num_frames/args.framerate)*1e6), frame)
        e = time.perf_counter()

        verify_frames.put(frame)
        pack_time += (e-s)
        num_frames += 1

        print("  Packed {} frames...".format(num_frames), end="\r")
        if args.num_frames is not None and num_frames == args.num_frames: break

    writer.close(write_frame_pos=not args.no_frame_pos)
    verify_frames.put(None) # stop verification
    verify_thread.join() # wait for it to finish
    if reader is not None:
        reader.close()

    print("Finished packing {} frames".format(num_frames))
    if num_frames > 0:
        print("Average pack time: {:.2f}ms".format(pack_time/num_frames*1000))
        print("Compression ratio: {:.2f}%".format(
            writer.file_size/(frame_size*num_frames)*100))
        if args.verify:
            print("Verify result:", ("success" if verify_result else "FAILURE"))
            if not verify_result:
                exit(1)

def main_unpack():
    parser = argparse.ArgumentParser(
        description="Unpack raw video data from a Vidpak file.")
    parser.add_argument('input', type=str,
        help="Path to Vidpak input file.")
    parser.add_argument('output', type=str,
        help="Path to raw output file (or - to write to stdout).")
    parser.add_argument('-n', '--num-frames', type=int,
        help="Only unpack the first n frames.")

    args = parser.parse_args()

    if args.num_frames is not None and args.num_frames <= 0:
        raise ValueError(f"number of frames {args.num_frames} must be positive")

    reader = VidpakFileReader(args.input)
    if args.output == "-":
        fout = sys.stdout.buffer
    else:
        fout = open(args.output, "wb")

    size = reader.size
    if fout is not sys.stdout.buffer:
        print("Frame size: {}x{}".format(*size))
        print("Tile size: {}x{}".format(*reader.tsize))

    empty_frames, full_frames = queue.Queue(), queue.Queue()
    for _ in range(4):
        empty_frames.put(np.empty((size[1], size[0]), dtype=np.uint16))
    frame_size = size[0]*size[1]*2

    def write_thread_fn():
        while True:
            frame = full_frames.get()
            if frame is None: break
            fout.write(frame)
            empty_frames.put(frame)
        fout.close()

    num_frames = 0
    unpack_time = 0
    write_thread = threading.Thread(target=write_thread_fn, daemon=True)
    write_thread.start()
    while True:
        frame = empty_frames.get()
        try:
            # time how long unpacking the frame takes
            s = time.perf_counter()
            timestamp, _, _ = reader.read_frame(num_frames, frame)
            e = time.perf_counter()
            unpack_time += (e-s)
        except IndexError: # out of frames
            break

        full_frames.put(frame)
        num_frames += 1

        if fout is not sys.stdout.buffer:
            print("  Unpacked {} frames...".format(num_frames), end="\r")
        if args.num_frames is not None and num_frames == args.num_frames: break

    reader.close()
    full_frames.put(None)
    write_thread.join()

    if fout is not sys.stdout.buffer:
        print("Finished unpacking {} frames".format(num_frames))
        if num_frames > 0:
            print("Average unpack time: {:.2f}ms".format(
                unpack_time/num_frames*1000))
            print("Compression ratio: {:.2f}%".format(
                reader.file_size/(frame_size*num_frames)*100))
