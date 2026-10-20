#!/usr/bin/env python3
"""Add a source file to XrtMac.xcodeproj's dext target.

The .xcodeproj is not in the repo (build.sh's rsync excludes it), so a new dext
source has to be registered on the Mac. Doing that by hand means four edits in
project.pbxproj with two invented UUIDs; this does it idempotently instead, so it
can be re-run and so the step is recorded rather than remembered.

    ./add_dext_source.py XrtFlashClient.iig XrtFlashClient.cpp
"""
import hashlib
import re
import sys
from pathlib import Path

PROJ = Path(__file__).with_name("XrtMac.xcodeproj") / "project.pbxproj"

KIND = {".cpp": "sourcecode.cpp.cpp", ".iig": "sourcecode.iig", ".h": "sourcecode.c.h"}


def uuid_for(name, salt):
    """A stable 24-hex id. Xcode only needs uniqueness, not randomness, and
    deriving it from the name keeps re-runs identical."""
    return hashlib.sha256((salt + name).encode()).hexdigest()[:24].upper()


def add(text, name):
    if f"/* {name} */" in text:
        print(f"  {name}: already registered")
        return text, False
    ext = Path(name).suffix
    if ext not in KIND:
        raise SystemExit(f"{name}: unhandled extension {ext}")
    build_id = uuid_for(name, "build")
    file_id = uuid_for(name, "file")

    # 1. PBXBuildFile, beside an existing one.
    anchor = re.search(r"\t\t(\w{24}) /\* XrtUserClient\.cpp in Sources \*/ = \{isa = PBXBuildFile;[^\n]*\n", text)
    assert anchor, "PBXBuildFile anchor not found"
    entry = (f"\t\t{build_id} /* {name} in Sources */ = {{isa = PBXBuildFile; "
             f"fileRef = {file_id} /* {name} */; }};\n")
    text = text[:anchor.end()] + entry + text[anchor.end():]

    # 2. PBXFileReference.
    anchor = re.search(r"\t\t\w{24} /\* XrtUserClient\.cpp \*/ = \{isa = PBXFileReference;[^\n]*\n", text)
    assert anchor, "PBXFileReference anchor not found"
    entry = (f"\t\t{file_id} /* {name} */ = {{isa = PBXFileReference; "
             f"lastKnownFileType = {KIND[ext]}; path = {name}; sourceTree = \"<group>\"; }};\n")
    text = text[:anchor.end()] + entry + text[anchor.end():]

    # 3. The group that lists the dext's files.
    anchor = re.search(r"\t\t\t\t\w{24} /\* XrtUserClient\.cpp \*/,\n", text)
    assert anchor, "group anchor not found"
    text = text[:anchor.end()] + f"\t\t\t\t{file_id} /* {name} */,\n" + text[anchor.end():]

    # 4. The Sources build phase. Only .cpp and .iig are compiled.
    if ext in (".cpp", ".iig"):
        anchor = re.search(r"\t\t\t\t\w{24} /\* XrtUserClient\.cpp in Sources \*/,\n", text)
        assert anchor, "Sources phase anchor not found"
        text = text[:anchor.end()] + f"\t\t\t\t{build_id} /* {name} in Sources */,\n" + text[anchor.end():]

    print(f"  {name}: added")
    return text, True


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if not PROJ.exists():
        raise SystemExit(f"{PROJ} not found - run this on the Mac")
    text = PROJ.read_text()
    changed = False
    for name in sys.argv[1:]:
        text, did = add(text, name)
        changed = changed or did
    if changed:
        PROJ.write_text(text)
        print(f"wrote {PROJ}")
    else:
        print("nothing to do")


if __name__ == "__main__":
    main()
