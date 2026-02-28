import logging

from .consts import *
from .corpus import *
from .gen_input_core import *
from .input_seq import *
from .cov import *
from .utils import *

logging.basicConfig(
    level=logging.INFO,
    format="[%(filename)18s:%(lineno)-3d] %(message)s",
)
