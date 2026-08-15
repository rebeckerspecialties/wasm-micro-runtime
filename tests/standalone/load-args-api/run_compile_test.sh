#!/bin/sh

set -eu

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../../.." && pwd)
output_dir=$(mktemp -d "${TMPDIR:-/tmp}/wamr-load-args-api.XXXXXX")
trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

c_compiler=${CC:-cc}
cxx_compiler=${CXX:-c++}
source_file="$test_dir/compile_load_args_layout.c"
include_dir="$repo_root/core/iwasm/include"

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [path-to-libiwasm]" >&2
    exit 2
fi

for component_model in undefined 0 1; do
    component_flag=
    if [ "$component_model" != undefined ]; then
        component_flag="-DWASM_ENABLE_COMPONENT_MODEL=$component_model"
    fi

    for header_mode in 1 2 3 4; do
        output_stem="$output_dir/load_args_${component_model}_${header_mode}"
        "$c_compiler" -std=c11 -Wall -Wextra -Werror -I "$include_dir" \
            -DTEST_HEADER_MODE="$header_mode" $component_flag \
            -c "$source_file" -o "${output_stem}.o"
        "$cxx_compiler" -x c++ -std=c++17 -Wall -Wextra -Werror \
            -I "$include_dir" -DTEST_HEADER_MODE="$header_mode" \
            $component_flag -c "$source_file" -o "${output_stem}.cc.o"
    done
done

if [ "$#" -eq 1 ]; then
    "$c_compiler" -std=c11 -Wall -Wextra -Werror -I "$include_dir" \
        "$test_dir/validate_load_args.c" "$1" -lpthread -lm \
        -o "$output_dir/validate_load_args"
    "$output_dir/validate_load_args"
fi

echo "LoadArgs public-header compile/layout checks passed"
