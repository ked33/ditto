"""Lightweight source checks; this does not replace the MSVC/MFC build."""

import re
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FILES = (
    "src/QPasteWnd.cpp", "src/QPasteWnd.h",
    "src/QPasteWndThread.cpp", "src/QPasteWndThread.h",
    "src/SearchIndex.cpp", "src/SearchIndexSql.cpp", "src/SearchIndexSql.h",
    "src/FormatSQL.cpp", "src/sqlite/CppSQLite3.cpp", "src/sqlite/CppSQLite3.h",
    "ICU_Loader/CharacterTokenizer.h", "ICU_Loader/icu.cpp",
)


def code_characters(source):
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find(chr(10), index)
            index = len(source) if end < 0 else end + 1
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            assert end >= 0, "Unclosed comment"
            index = end + 2
        elif source[index] in (chr(34), chr(39)):
            quote = source[index]
            index += 1
            while index < len(source):
                char = source[index]
                index += 1
                if ord(char) == 92:
                    index += 1
                elif char == quote:
                    break
            else:
                raise AssertionError("Unclosed string/character literal")
        else:
            yield source[index]
            index += 1


def main():
    for name in FILES:
        stack = []
        for char in code_characters((ROOT / name).read_text(encoding="utf-8-sig")):
            if char in "({[":
                stack.append(char)
            elif char in ")}]":
                assert stack and stack.pop() == "({["[")}]".index(char)], name
        assert not stack, (name, stack)
    print(f"C++ delimiter checks: {len(FILES)} files passed")

    namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    project = ET.parse(ROOT / "CP_Main.vcxproj")
    compiled = {element.attrib.get("Include", "") for element in project.findall(".//m:ClCompile", namespace)}
    for stem in ("SearchIndex", "SearchIndexSql", "FormatSQL", "QPasteWndThread", "QPasteWnd"):
        assert any(path.endswith(stem + ".cpp") for path in compiled), stem
    print("MSBuild source registrations: passed")

    for filename in ("ICU_Loader/ICU_Loader.vcxproj", "ICU_Loader/ICU_Loader.vcxproj.filters"):
        loader = ET.parse(ROOT / filename)
        headers = {item.attrib.get("Include") for item in loader.findall(".//m:ClInclude", namespace)}
        assert "CharacterTokenizer.h" in headers, filename
    print("ICU tokenizer project registrations: passed")

    header = (ROOT / "src/QPasteWndThread.h").read_text()
    implementation = (ROOT / "src/QPasteWndThread.cpp").read_text()
    window = (ROOT / "src/QPasteWnd.cpp").read_text(encoding="utf-8-sig")
    for method in ("CancelSearch", "ResumeSearch", "AcknowledgeListCount",
                   "SetSearchSql", "BeginSearchConnection", "EndSearchConnection",
                   "SearchProgress", "SearchBusy", "FinishSearch", "FireLoadItemsRequest"):
        assert method in header, method
        assert f"CQPasteWndThread::{method}(" in implementation, method
    assert "ON_MESSAGE(NM_SEARCH_RESULTS_READY, OnSearchResultsReady)" in window
    assert "m_bStopQuery" not in window + implementation
    assert "m_request.countNeeded = false;" in implementation
    print("Search worker declarations and message registration: passed")

    definitions = (ROOT / "src/QListCtrl.h").read_text()
    messages = re.findall(r"#define\s+(NM_\w+)\s+WM_USER\s*\+\s*(0x[0-9a-fA-F]+)", definitions)
    values = [int(value, 16) for _, value in messages]
    assert len(values) == len(set(values)), "Duplicate list-control message ID"
    print("List-control message IDs: unique")


if __name__ == "__main__":
    main()
