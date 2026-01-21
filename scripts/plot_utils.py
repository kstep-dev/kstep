from pathlib import Path

from consts import RESULTS_DIR
from matplotlib.figure import Figure


def save_fig(fig: Figure, filename: Path | str) -> list[Path]:
    path = Path(filename) if isinstance(filename, str) else filename
    path = path if path.is_absolute() else RESULTS_DIR / path
    paths = [path.with_suffix(f".{ext}") for ext in ["pdf", "png"]]

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
