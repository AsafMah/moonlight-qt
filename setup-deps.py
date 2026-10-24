import os
import sys
import platform
import urllib.request
import zipfile
import shutil
import subprocess

ORGANIZATION = "moonlight-stream"
PREBUILT_REPO = "moonlight-qt-deps"
TAG = "v19"

def get_platform_config():
    system = platform.system()
    if system == "Darwin":
        return "mac", "macos-universal.zip"
    if system == "Linux":
        return "steamlink", "steamlink.zip"

    print(f"Error: Unsupported platform ({system})")
    sys.exit(1)

def download_and_extract():
    subfolder, asset_name = get_platform_config()
    
    target_dir = os.path.join(os.getcwd(), "libs", subfolder)
    url = f"https://github.com/{ORGANIZATION}/{PREBUILT_REPO}/releases/download/{TAG}/{asset_name}"

    if os.path.exists(target_dir):
        print("Cleaning target directory...")
        shutil.rmtree(target_dir)

    os.makedirs(target_dir, exist_ok=True)

    archive_path = os.path.join(target_dir, asset_name)

    print(f"Downloading {asset_name}...")
    try:
        urllib.request.urlretrieve(url, archive_path)
    except Exception as e:
        print(f"Download failed: {e}")
        sys.exit(1)

    print(f"Extracting {asset_name}...")
    with zipfile.ZipFile(archive_path, 'r') as zip_ref:
        zip_ref.extractall(target_dir)

    os.remove(archive_path)

    if subfolder == "mac":
        build_pyrowave_runtime(os.path.join(target_dir, "lib"))

    print(f"Dependencies successfully deployed")

# PyroWave on macOS is a later phase: Granite, which the runtime builds on,
# doesn't compile on macOS yet (TIMER_ABSTIME in util/timer.cpp). Until it
# does, skip the runtime whatever PYROWAVE_BUILD says. The app hides PyroWave
# when the runtime is missing.
BUILD_PYROWAVE_ON_MACOS = False

def build_pyrowave_runtime(lib_dir):
    if not BUILD_PYROWAVE_ON_MACOS:
        print("PyroWave runtime skipped on macOS: Granite doesn't build on macOS yet, "
              "so PyroWave stays hidden in this build")
        return

    # Builds a universal libpyrowave-shared.101.dylib into libs/mac/lib, which
    # app.pro copies into the bundle's Frameworks directory. PYROWAVE_BUILD
    # (auto, require or skip) is read by the script itself.
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scripts", "build-pyrowave.sh")
    result = subprocess.run(["bash", script, lib_dir, "universal"])
    if result.returncode != 0:
        sys.exit(result.returncode)

if __name__ == "__main__":
    download_and_extract()