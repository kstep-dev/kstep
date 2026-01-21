from pathlib import Path

from consts import RESULTS_DIR
from matplotlib.figure import Figure


def save_fig(fig: Figure, filename: Path | str):
    if isinstance(filename, str):
        filename = RESULTS_DIR / filename

    paths = [filename.with_suffix(f".{ext}") for ext in ["pdf", "png"]]

    fig.tight_layout(pad=0)
    for path in paths:
        fig.savefig(
            path,
            bbox_inches="tight",
            pad_inches=0,
            dpi=1000,
            metadata={"CreationDate": None},  # for reproducible builds
        )
        print(f"Saved figure to {path}")
    return paths
