#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

for f in $(ls *.plantuml); do plantuml -tsvg $f && rsvg-convert -f pdf -o $(basename $f .plantuml).pdf $(basename $f .plantuml).svg; done
