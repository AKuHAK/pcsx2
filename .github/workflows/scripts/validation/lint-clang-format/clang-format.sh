#!/bin/bash
# Test clang-format
clangformatout=$(git clang-format-12 --style=file HEAD^ --diff)

# Redirect output to stderr.
exec 1>&2

if [ "$clangformatout" != "" ]
then
    echo "Format error!"
    echo "Use git clang-format"
    exit 1
fi
