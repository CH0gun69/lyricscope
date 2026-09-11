import os

# Qt needs a platform plugin even for widget construction; offscreen keeps
# the suite headless and CI-safe.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
