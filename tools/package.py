import argparse
import hashlib
import os
import sys
import zipfile


def main():
    parser = argparse.ArgumentParser(description="Zip a staged plugin folder (extract-into-the-game-folder layout)")
    parser.add_argument("stage", help="folder whose contents become the zip root")
    parser.add_argument("zip_path", help="zip file to write; a .sha256 file is written next to it")
    args = parser.parse_args()
    if not os.path.isdir(args.stage):
        sys.exit("%s is not a folder: run `make dist` first" % args.stage)
    entries = []
    for root, dirs, files in os.walk(args.stage):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(root, name)
            entries.append((full, os.path.relpath(full, args.stage).replace(os.sep, "/")))
    os.makedirs(os.path.dirname(os.path.abspath(args.zip_path)), exist_ok=True)
    with zipfile.ZipFile(args.zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for full, arc in entries:
            info = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())
    digest = hashlib.sha256(open(args.zip_path, "rb").read()).hexdigest()
    with open(args.zip_path + ".sha256", "w") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(args.zip_path)))
    for _full, arc in entries:
        print(arc)
    print("wrote %s (sha256 %s)" % (args.zip_path, digest))


if __name__ == "__main__":
    main()
