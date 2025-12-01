from pathlib import Path

from matplotlib.figure import Figure


def save_fig(fig: Figure, filename: Path):
    fig.tight_layout()
    pdf_path = filename.with_suffix(".pdf")
    png_path = filename.with_suffix(".png")

    fig.savefig(pdf_path, bbox_inches="tight", pad_inches=0, dpi=1000)
    fig.savefig(png_path, bbox_inches="tight", pad_inches=0, dpi=1000)

    print(f"Saved figure to {pdf_path} and {png_path}")

    return pdf_path, png_path
