#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

if [ ! -e ./LICENSE ] || [ ! -e ./README.md ]; then
  echo "Please run this script from the project root!"
  exit 1
fi

SPDX_LICENSE_STR="SPDX-License-Identifier: "
echo 'Searching for SPDX expression "'"$SPDX_LICENSE_STR"'"'

declare -a MISSING_SPDX_FILES
FILES=$(find . \
  -type f \( -name "*.[ch]" -o -name "*.nix" -o -name "*.options" -o -name "*.proto" -o -name "*.py" -o -name "*.rs" -o -name "*.sh" \) \
  -a -not -path '*/.*' \
  -a -not -path './doc/*' \
  -a -not -path './external/*' \
  -a -not -path '*/generated/*' \
  -print
)

while read file; do 
  head -n 2 "$file" | grep -q --fixed-strings "$SPDX_LICENSE_STR"
  if [ $? -ne 0 ]; then
    MISSING_SPDX_FILES+=("$file")
  fi
done <<< "$FILES"

grep -q --fixed-strings "$SPDX_LICENSE_STR" README.md
if [ $? -ne 0 ]; then
  MISSING_SPDX_FILES+=("README.md")
fi

if [ ${#MISSING_SPDX_FILES[@]} -gt 0 ]; then
  echo
  echo "SPDX expression not found in the following files:"
  echo
  printf '%s\n' "${MISSING_SPDX_FILES[@]}"
  exit 1
else
  echo "Looks good :-)"
fi

