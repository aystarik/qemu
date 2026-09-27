"""
Shared helpers for the BCM56870 harness.

Resolves the two external inputs the harness needs:

  * the QEMU binary   -- built from the surrounding QEMU tree
  * the firmware image -- NOT part of this repository (it is a vendor binary),
                          so it is located by searching a few known places.

The image can always be pointed at explicitly with the BCM56870_IMAGE
environment variable, which takes precedence over the search.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# qemu/bcm56870/harness -> qemu/bcm56870 -> qemu
BCM_DIR = os.path.dirname(HERE)
QEMU_TREE = os.path.dirname(BCM_DIR)

QEMU = os.environ.get("BCM56870_QEMU") or os.path.join(
    QEMU_TREE, "build", "qemu-system-arm")

IMAGE_NAME = "BCM56870_0_bfd_cortex-r5.bin"

# Search order: an explicit copy dropped in bcm56870/firmware/, then the
# directory this checkout happens to sit in, then its parent.
_IMAGE_CANDIDATES = [
    os.path.join(BCM_DIR, "firmware", IMAGE_NAME),
    os.path.join(os.path.dirname(QEMU_TREE), IMAGE_NAME),
    os.path.join(os.path.dirname(os.path.dirname(QEMU_TREE)), IMAGE_NAME),
]


def find_image():
    env = os.environ.get("BCM56870_IMAGE")
    if env:
        if not os.path.exists(env):
            raise SystemExit(f"BCM56870_IMAGE={env!r} does not exist")
        return os.path.abspath(env)
    for c in _IMAGE_CANDIDATES:
        if os.path.exists(c):
            return os.path.abspath(c)
    raise SystemExit(
        "could not find the firmware image.\n"
        f"Looked in:\n" + "".join(f"  {c}\n" for c in _IMAGE_CANDIDATES) +
        "Set BCM56870_IMAGE=/path/to/" + IMAGE_NAME + " to point at it.")


IMAGE = find_image()
