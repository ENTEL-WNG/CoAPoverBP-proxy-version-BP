#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

# Script to build the Docker images necessary for running uD3TN's CI
# tests. Run this from the uD3TN base directory!

set -euo pipefail

if [[ ! -r test/dockerfiles/ion-interop ]]; then
    echo "Please run this script from the uD3TN base directory!"
    exit 1
fi

docker build . \
    --file test/dockerfiles/ion-interop \
    --build-arg ARCHIVE_URL="https://sourceforge.net/projects/ion-dtn/files/ion-open-source-3.7.4.tar.gz/download?use_mirror=netcologne&ts=$(date +%s)" \
    --tag ud3tn-ion-interop:3.7.4

docker build . \
    --file test/dockerfiles/ion-interop \
    --build-arg ARCHIVE_URL="https://github.com/nasa-jpl/ION-DTN/archive/refs/tags/ion-open-source-4.1.3.tar.gz" \
    --tag ud3tn-ion-interop:4.1.3

docker build . \
    --file test/dockerfiles/ione-interop \
    --build-arg ARCHIVE_URL="https://sourceforge.net/projects/ione/files/ione-1.1.0.tar.gz/download?use_mirror=netcologne&ts=$(date +%s)" \
    --tag ud3tn-ione-interop:1.1.0

docker build . \
    --file test/dockerfiles/hdtn-interop \
    --build-arg GIT_REF="v1.3.1" \
    --tag ud3tn-hdtn-interop:1.3.1

docker build . \
    --file test/dockerfiles/dtn7-interop \
    --build-arg GIT_REF="b3a6065" \
    --tag ud3tn-dtn7-interop:0.21.0-b3a6065

docker build . \
    --file test/dockerfiles/ci-python-clang \
    --build-arg IMAGE=python:3.8-bookworm \
    --tag ci-python-clang:3.8-bookworm

docker build . \
    --file test/dockerfiles/ci-python-clang \
    --build-arg IMAGE=python:3.12-bookworm \
    --tag ci-python-clang:3.12-bookworm

echo "You may now push the images to the registry:"
echo "\$ docker tag ud3tn-ion-interop:<tag> registry.gitlab.com/d3tn/ud3tn-docker-images/ion-interop:<tag>"
echo "\$ docker login registry.gitlab.com"
echo "\$ docker push registry.gitlab.com/d3tn/ud3tn-docker-images/ion-interop:<tag>"
