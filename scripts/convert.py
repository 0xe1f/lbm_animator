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


def strip_extras(content):
    return content \
        .strip() \
        .removeprefix("CanvasCycle.initScene(") \
        .removesuffix(");")


def size_chunk_header():
    return 4 + 4 # 4 bytes for header, 4 for length


def write_chunk_header(output_file, name, size):
    print(f"Writing {name} ({size} bytes)")
    output_file.write(name.encode())
    output_file.write(struct.pack('>I', size))


def write_form(output_file, data):
    colors = data['colors']
    cycles = data['cycles']
    pixels = data['pixels']
    format_type = b'PBM '

    form_size = len(format_type) + \
        size_chunk_header() + size_bmhd(data) + \
        size_chunk_header() + size_cmap(colors) + \
        sum([size_chunk_header() + size_crng(cycle) for cycle in cycles]) + \
        size_chunk_header() + size_body(pixels)

    write_chunk_header(output_file, 'FORM', form_size)
    output_file.write(format_type)
    write_bmhd(output_file, data)
    write_cmap(output_file, colors)
    for cycle in cycles:
        write_crng(output_file, cycle)
    write_body(output_file, pixels)


def size_bmhd(_):
    return 20


def write_bmhd(output_file, data):
    write_chunk_header(output_file, 'BMHD', size_bmhd(data))

    output_file.write(struct.pack('>H', data['width']))  # width
    output_file.write(struct.pack('>H', data['height'])) # height
    output_file.write(struct.pack('>h', 0)) # x_origin
    output_file.write(struct.pack('>h', 0)) # y_origin
    output_file.write(struct.pack('>B', 8)) # num_planes
    output_file.write(struct.pack('>B', 0)) # mask
    output_file.write(struct.pack('>B', 0)) # compression
    output_file.write(struct.pack('>B', 0)) # pad1
    output_file.write(struct.pack('>H', 0)) # trans_clr
    output_file.write(struct.pack('>B', 1)) # x_aspect
    output_file.write(struct.pack('>B', 1)) # y_aspect
    output_file.write(struct.pack('>h', data['width']))  # page_width
    output_file.write(struct.pack('>h', data['height'])) # page_height


def size_cmap(colors):
    return len(colors) * 3


def write_cmap(output_file, colors):
    write_chunk_header(output_file, 'CMAP', size_cmap(colors))
    for color in colors:
        output_file.write(struct.pack('>B', color[0])) # r
        output_file.write(struct.pack('>B', color[1])) # g
        output_file.write(struct.pack('>B', color[2])) # b


def size_crng(_):
    return 8


def write_crng(output_file, data):
    write_chunk_header(output_file, 'CRNG', size_crng(data))

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


def size_body(data):
    return len(data)


def write_body(output_file, data):
    write_chunk_header(output_file, 'BODY', size_body(data))

    output_file.write(bytes(data))


def write_lbm(output_file, data):
    write_form(output_file, data)


def main():
    parser = argparse.ArgumentParser(description="CanvasCycle converter")
    parser.add_argument("js_file", help="Path to the JavaScript/JSON file")
    parser.add_argument("output_file", help="Path to the output LBM file")
    args = parser.parse_args()

    with open(args.js_file, 'r') as f:
        content = f.read()

    content = strip_extras(content)
    data = json.loads(content)

    with open(args.output_file, 'wb') as f:
        write_lbm(f, data['base'])


if __name__ == "__main__":
    main()
