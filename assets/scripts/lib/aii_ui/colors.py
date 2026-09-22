"""Named colours for `aii_ui` panels, and the one mapping that matters:
turning a status word into a colour.

This file is small on purpose. If you want a colour this module does not
have, add it here as an `(r, g, b, a)` tuple of 0..1 sRGB floats -- the same
convention `aii.ui.text_colored` and friends take -- rather than writing a
raw tuple inline at the call site. Keeping the names in one place is what
lets `for_state` and every panel agree on what "orange" means.
"""

ORANGE = (0.95, 0.60, 0.10, 1.0)
RED = (0.90, 0.25, 0.20, 1.0)
GREEN = (0.30, 0.80, 0.35, 1.0)
GREY = (0.60, 0.60, 0.60, 1.0)
WHITE = (1.0, 1.0, 1.0, 1.0)
BLUE = (0.30, 0.55, 0.95, 1.0)
YELLOW = (0.95, 0.90, 0.20, 1.0)

# Words seen in the wild for each bucket. Extend these sets rather than
# adding new buckets, unless you also teach `StatusPanel` about the new one.
_ORANGE_WORDS = {"pending", "building", "running"}
_RED_WORDS = {"failed", "error"}
_GREEN_WORDS = {"done", "ok", "complete"}


def for_state(state):
    """Map a status word to a colour: orange while it is happening, red when
    it failed, green when it finished, and grey for anything this module
    does not recognise -- an unknown word is not an error, just untinted."""
    word = (state or "").strip().lower()
    if word in _ORANGE_WORDS:
        return ORANGE
    if word in _RED_WORDS:
        return RED
    if word in _GREEN_WORDS:
        return GREEN
    return GREY
