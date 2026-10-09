"""Validate theme colors and the XML files shipped in a portable artifact."""

import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COLOR_KEYS = ("ScrollBarTrack", "ScrollBarThumb", "ScrollBarThumbHover")


def check_theme(path):
    root = ET.parse(path).getroot()
    assert root.tag == "Ditto_Theme_File", path
    colors = []
    for key in COLOR_KEYS:
        nodes = root.findall(key)
        assert len(nodes) == 1, f"{path}: expected one {key}"
        match = re.fullmatch(
            r"RGB\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)",
            (nodes[0].text or "").strip(),
            re.IGNORECASE,
        )
        assert match, f"{path}: invalid {key}"
        color = tuple(map(int, match.groups()))
        assert all(0 <= channel <= 255 for channel in color), (path, key)
        colors.append(color)
    assert len(set(colors)) == 3, f"{path}: scrollbar states must be distinct"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", type=Path, help="Directory containing Ditto.exe")
    args = parser.parse_args()

    # Packaging uses the tracked Debug resources, not the root Themes directory.
    script = (ROOT / "DittoSetup/BuildPortableZIP.bat").read_text()
    assert re.search(
        r"^copy\s+\.\.\\debug\\themes\\\*\.xml\s+ditto\\themes\\\s*$",
        script, re.IGNORECASE | re.MULTILINE,
    ), "Theme packaging source changed; update this check to validate the new source"

    shipped = sorted((ROOT / "Debug/Themes").glob("*.xml"))
    root_themes = sorted((ROOT / "Themes").glob("*.xml"))
    assert shipped and root_themes, "Missing theme resources"
    for path in shipped + root_themes:
        check_theme(path)
    print(f"Theme XML and scrollbar colors: {len(shipped)} shipped + {len(root_themes)} root themes passed")

    if args.package_dir is not None:
        packaged = args.package_dir / "Themes"
        expected_names = {path.name for path in shipped}
        actual_names = {path.name for path in packaged.glob("*.xml")}
        assert actual_names == expected_names, "Portable artifact has missing or unexpected themes"
        for source in shipped:
            target = packaged / source.name
            assert target.read_bytes() == source.read_bytes(), f"Stale packaged theme: {source.name}"
        print(f"Portable artifact theme contents: {len(shipped)} files match the packaging source")


if __name__ == "__main__":
    main()
