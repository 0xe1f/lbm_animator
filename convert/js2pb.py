#! /usr/bin/env python3

# Copyright (C) 2024 Akop Karapetyan
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from compression import gzip
import argparse
import json
import re
import scene_pb2
import google.protobuf as pb

def main():
    # protoc -Iproto --python_out=. user.proto

    parser = argparse.ArgumentParser(description='Extract data from a canvascycle JSON file')
    parser.add_argument('file', type=str, help="Path to the input JSON file")
    parser.add_argument('-o', '--out', type=str, default=None, help="Path to the output protobuf file (optional)")
    parser.add_argument('-f', '--format', type=str, choices=['bin', 'txt'], default='bin', help="Output format: 'bin' for binary protobuf, 'txt' for text protobuf (default: 'bin')")
    parser.add_argument('-c', '--compress', type=str, choices=['gz', 'none'], default='none', help="Compression format: 'gz' for gzip, 'none' for no compression (default: 'none')")
    args = parser.parse_args()

    with open(args.file, 'r') as f:
        content = f.read()
        content = content.split("(", 1)[-1].rsplit(")", 1)[0]
        content = re.sub(r"(\w+):", r"'\1':", content).replace("'", '"')
        data = json.loads(content)

    scene = scene_pb2.Scene()
    scene.width = int(data["width"])
    scene.height = int(data["height"])
    scene.pixels = bytes(data["pixels"])

    colors = []
    for color in data["colors"]:
        r, g, b = color
        colors.append((r << 16) | (g << 8) | b)
    scene.palette.extend(colors)

    cycles = []
    for cycle in data["cycles"]:
        cycle_msg = scene.cycles.add()
        cycle_msg.flags = cycle["reverse"]
        cycle_msg.rate = cycle["rate"]
        cycle_msg.low = cycle["low"]
        cycle_msg.high = cycle["high"]
        cycles.append(cycle_msg)
    scene.cycles.extend(cycles)

    name = data["filename"].rsplit(".", 1)[0].lower()
    filename = args.out if args.out else f'{name}.pbbin' if args.format == 'bin' else f'{name}.pbtxt'
    if args.format == 'bin':
        if args.compress == 'gz':
            with gzip.open(f"{filename}.gz", 'wb') as f:
                f.write(scene.SerializeToString())
        else:
            with open(filename, 'wb') as f:
                f.write(scene.SerializeToString())
    elif args.format == 'txt':
        if args.compress == 'gz':
            with gzip.open(f"{filename}.gz", 'wt') as f:
                f.write(pb.text_format.MessageToString(scene))
        else:
            with open(filename, 'w') as f:
                f.write(pb.text_format.MessageToString(scene))

    print(f"Scene data extracted and saved to {filename} as {'binary' if args.format == 'bin' else 'text'}.")

if __name__ == '__main__':
    main()
