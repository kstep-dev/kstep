from pathlib import Path

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"
LINUX_MASTER_DIR = LINUX_ROOT_DIR / "master"
LINUX_CURR_DIR = LINUX_ROOT_DIR / "current"

RESULTS_DIR = PROJ_DIR / "results"
DATA_DIR = PROJ_DIR / "data"
DOWNLOAD_DIR = DATA_DIR / "download"
BUILD_DIR = PROJ_DIR / "build"
LOGS_DIR = DATA_DIR / "logs"
QEMU_DIR = DATA_DIR / "qemu"
CORPUS_DIR = DATA_DIR / "corpus"

LATEST_LOG = DATA_DIR / "latest.log"
LATEST_OUTPUT = DATA_DIR / "latest.jsonl"
LATEST_COV = DATA_DIR / "latest.cov"
LATEST_COV_JSON = DATA_DIR / "latest.cov.json"


def update_latest(latest_file: Path, new_file: Path):
    assert latest_file in (
        LATEST_LOG,
        LATEST_OUTPUT,
        LATEST_COV,
        LATEST_COV_JSON,
    )
    new_file.touch()
    latest_file.unlink(missing_ok=True)
    latest_file.symlink_to(new_file)
