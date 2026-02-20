import logging

from .consts import *
from .gen_input_core import *
from .kcov_symbolize import *
from .kcov_parse_signal import *
from .utils import *

logging.basicConfig(
    level=logging.INFO,
    format="[%(filename)18s:%(lineno)-3d] %(message)s",
)
