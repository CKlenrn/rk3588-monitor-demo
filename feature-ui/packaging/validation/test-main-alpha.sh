#!/bin/sh

set -eu

script_dir="$(CDPATH= cd "$(dirname "$0")" && pwd)"
source_file="$script_dir/../../main.cpp"

awk '
/^[[:space:]]*QQuickWindow::setDefaultAlphaBuffer[(]true[)];[[:space:]]*$/ {
    default_alpha_count++
    default_alpha_line = NR
}
/^[[:space:]]*QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat[(][)];[[:space:]]*$/ {
    surface_format_count++
    surface_format_line = NR
}
/^[[:space:]]*surfaceFormat[.]setAlphaBufferSize[(]8[)];[[:space:]]*$/ {
    alpha_size_count++
    alpha_size_line = NR
}
/^[[:space:]]*QSurfaceFormat::setDefaultFormat[(]surfaceFormat[)];[[:space:]]*$/ {
    default_format_count++
    default_format_line = NR
}
/^[[:space:]]*QGuiApplication app[(]argc, argv[)];[[:space:]]*$/ {
    application_count++
    application_line = NR
}
END {
    if (default_alpha_count != 1 || surface_format_count != 1 || alpha_size_count != 1 || default_format_count != 1 || application_count != 1) {
        print "FAIL: expected each Qt alpha initialization statement exactly once"
        exit 1
    }
    if (default_alpha_line >= application_line || surface_format_line >= alpha_size_line || alpha_size_line >= default_format_line || default_format_line >= application_line) {
        print "FAIL: Qt alpha defaults must be configured before QGuiApplication"
        exit 1
    }
    print "PASS: Qt alpha defaults precede QGuiApplication"
}
' "$source_file"
