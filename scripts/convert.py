#!/usr/bin/env python3

# Copyright (c) 2026 Akop Karapetyan
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Converts CanvasCycle-formatted JS files to LBM/IFF format

import json
import argparse
import struct
import re
import sys

log_verbose = False


def jsonify(content):
    # Remove the function call wrapper and convert to JSON format
    strip_affixes = content \
        .strip() \
        .removeprefix("CanvasCycle.initScene(") \
        .removesuffix(");")
    # Convert unquoted keys to quoted keys
    quote_keys = re.sub(r"(\w+):", r'"\1":', strip_affixes)
    # Convert single-quoted strings to double-quoted strings
    return re.sub(r"'([^,}\]]+)'", r'"\1"', quote_keys)


def write_chunk_header(output_file, name, chunk_size):
    header_size = 8
    if log_verbose:
        if chunk_size == 0xffff_ffff:
            print(f"Writing {name} (???? bytes)")
        else:
            print(f"Writing {name} ({chunk_size} bytes)")

    output_file.write(name.encode())
    output_file.write(struct.pack('>I', chunk_size))

    return header_size


def pad_chunk(output_file, size):
    if size % 2 == 1:
        output_file.write(b'\x00')
        size += 1

    return size


def write_form(output_file, data, type='FORM', form_type='PBM '):
    colors = data['colors']
    cycles = data['cycles']
    pixels = bytes(data['pixels'])
    name = data.get('filename', '')
    compressed_pixels = rle_compress(pixels)
    format = form_type.encode()

    # Write FORM header and format
    header_size = write_chunk_header(output_file, type, 0xffff_ffff)
    pos = output_file.tell()
    pad_chunk(output_file, output_file.write(format))

    # Write chunks and build total size
    pad_chunk(output_file, write_bmhd(output_file, data))
    pad_chunk(output_file, write_cmap(output_file, colors))
    for cycle in cycles:
        pad_chunk(output_file, write_crng(output_file, cycle))
    pad_chunk(output_file, write_body(output_file, compressed_pixels))
    if name:
        pad_chunk(output_file, write_text_chunk(output_file, name))

    end_pos = output_file.tell()
    size = end_pos - pos

    output_file.seek(pos - 4) # Seek to size field in FORM header
    output_file.write(struct.pack('>I', size)) # Write total size
    if log_verbose:
        print(f"Updated {type} size to {size} bytes")

    output_file.seek(end_pos) # Seek back to end of file

    return size + header_size


def write_bmhd(output_file, data):
    chunk_size = 20
    write_chunk_header(output_file, 'BMHD', chunk_size)

    output_file.write(struct.pack('>H', data['width']))  # width
    output_file.write(struct.pack('>H', data['height'])) # height
    output_file.write(struct.pack('>h', 0)) # x_origin
    output_file.write(struct.pack('>h', 0)) # y_origin
    output_file.write(struct.pack('>B', 8)) # num_planes
    output_file.write(struct.pack('>B', 0)) # mask
    output_file.write(struct.pack('>B', 1)) # compression
    output_file.write(struct.pack('>B', 0)) # pad1
    output_file.write(struct.pack('>H', 0)) # trans_clr
    output_file.write(struct.pack('>B', 1)) # x_aspect
    output_file.write(struct.pack('>B', 1)) # y_aspect
    output_file.write(struct.pack('>h', data['width']))  # page_width
    output_file.write(struct.pack('>h', data['height'])) # page_height

    return chunk_size


def write_cmap(output_file, colors):
    size = len(colors) * 3
    write_chunk_header(output_file, 'CMAP', size)

    for color in colors:
        output_file.write(struct.pack('>B', color[0])) # r
        output_file.write(struct.pack('>B', color[1])) # g
        output_file.write(struct.pack('>B', color[2])) # b

    return size


def write_crng(output_file, data):
    size = 8
    write_chunk_header(output_file, 'CRNG', size)

    flags = 0
    if data['reverse'] == 2:
        flags |= 2
    if data['rate'] != 0:
        flags |= 1

    output_file.write(struct.pack('>H', 0)) # padding
    output_file.write(struct.pack('>H', data['rate'])) # rate
    output_file.write(struct.pack('>H', flags)) # flags
    output_file.write(struct.pack('>B', data['low']))  # low
    output_file.write(struct.pack('>B', data['high'])) # high

    return size


def write_body(output_file, byte_data):
    size = len(byte_data)
    write_chunk_header(output_file, 'BODY', size)

    output_file.write(byte_data)

    return size


def write_text_chunk(output_file, text, chunk_name='NAME'):
    size = len(text)
    write_chunk_header(output_file, chunk_name, size)

    output_file.write(text.encode())

    return size


def write_lbm(output_file, data):
    write_form(output_file, data)


def rle_compress(byte_data):
    compressed = bytearray()
    i = 0
    n = len(byte_data)

    while i < n:
        # Count repeated run from current position.
        run_value = byte_data[i]
        run_len = 1
        while i + run_len < n and byte_data[i + run_len] == run_value and run_len < 128:
            run_len += 1

        if run_len >= 2:
            # Repeat packet: control byte in [129..255], count = 257 - control.
            compressed.append(257 - run_len)
            compressed.append(run_value)
            i += run_len
            continue

        # Literal packet: gather bytes until next repeat-run starts or limit reached.
        literal_start = i
        i += 1
        while i < n and (i - literal_start) < 128:
            next_run_len = 1
            next_value = byte_data[i]
            while i + next_run_len < n and byte_data[i + next_run_len] == next_value and next_run_len < 128:
                next_run_len += 1

            # Stop literals before a repeat packet to allow better compression.
            if next_run_len >= 2:
                break
            i += 1

        literal_len = i - literal_start
        compressed.append(literal_len - 1)
        compressed.extend(byte_data[literal_start:i])

    return bytes(compressed)


def main():
    parser = argparse.ArgumentParser(description="CanvasCycle converter")
    parser.add_argument('js_file', help="Path to the JavaScript/JSON file")
    parser.add_argument('--verbose', '-v', help="Enable verbose output", action="store_true")
    parser.add_argument('output_file', help="Path to the output LBM file", default=None, nargs='?')
    args = parser.parse_args()

    global log_verbose
    log_verbose = args.verbose
    output_file = args.output_file
    if output_file is None:
        output_file = args.js_file.rsplit('.', 1)[0] + '.lbm'

    # Read and parse the input file
    with open(args.js_file, 'r') as f:
        content = f.read()

    # Convert to JSON and parse
    content = jsonify(content)
    data = json.loads(content)

    # Write the output file
    print(f"Writing to {output_file}...", file=sys.stderr)
    with open(output_file, 'wb') as f:
        write_lbm(f, data['base'])

    print("Done!", file=sys.stderr)


# Next:
#   - write LIST
if __name__ == "__main__":
    main()
