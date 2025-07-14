import os
import shutil
import re

SOURCE_DIRS = {
    "utils": "utils",
    "http": "http"
}
DEST_ROOT = "include/wfx"

EXPORT_REGEX = re.compile(r"\bWFX_API\b")

def ensure_clean_dir(path):
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(path)

def should_export_file(filepath):
    with open(filepath, "r", encoding="utf-8") as f:
        return any(EXPORT_REGEX.search(line) for line in f)

def copy_exported_files():
    for ns, subdir in SOURCE_DIRS.items():
        src_dir = os.path.abspath(subdir)
        dest_dir = os.path.join(DEST_ROOT, ns)

        ensure_clean_dir(dest_dir)

        for root, _, files in os.walk(src_dir):
            for file in files:
                if not file.endswith(".hpp"):
                    continue

                full_path = os.path.join(root, file)
                if should_export_file(full_path):
                    shutil.copy2(full_path, os.path.join(dest_dir, file))
                    print(f"[+] Exported: {file} -> wfx/{ns}/")

# def copy_export_signature():
#     sig_path = "include/export_signature.hpp"
#     target_path = os.path.join("include", "wfx", "export_signature.hpp")

#     os.makedirs(os.path.dirname(target_path), exist_ok=True)
#     shutil.copy2(sig_path, target_path)
#     print("[*] Copied export_signature.hpp")

if __name__ == "__main__":
    print("[*] Generating public WFX headers...")
    copy_exported_files()
    # copy_export_signature()
    print("[✓] Done.")