import logging

from .consts import *
from .utils import *
from .gen_input_core import *

logging.basicConfig(
    level=logging.INFO,
    format="[%(filename)18s:%(lineno)-3d] %(message)s",
)
