#!/bin/bash

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

# . convert/prep.sh && convert/js2pb.py js/V05RAIN.js 

set -e

pushd "$(dirname "$0")" > /dev/null

if [ ! -f ".venv/bin/activate" ]; then
    echo "Setting up virtual environment..."
    python3 -m venv .venv
    source .venv/bin/activate
    pip install -r requirements.txt
else
    source .venv/bin/activate
fi

if [ ! -f scene_pb2.py ] || [ "../proto/scene.proto" -nt "scene_pb2.py" ]; then
    echo "Generating scene_pb2.py from scene.proto"
    protoc -I../proto --python_out=. scene.proto
fi

popd > /dev/null
