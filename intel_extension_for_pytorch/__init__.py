import torch
try:
    import torchvision
except ImportError:
    pass  # skip if torchvision is not available

from .version import __version__
from .utils import _cpu_isa
_cpu_isa.check_minimal_isa_support()

torch_version = ''
ipex_version = ''
import re
matches = re.match('(\d+\.\d+).*', torch.__version__)
if matches and len(matches.groups()) == 1:
  torch_version = matches.group(1)
matches = re.match('(\d+\.\d+).*', __version__)
if matches and len(matches.groups()) == 1:
  ipex_version = matches.group(1)
if torch_version == '' or ipex_version == '' or torch_version != ipex_version:
  print('ERROR! Intel® Extension for PyTorch* needs to work with PyTorch {0}.*, but PyTorch {1} is found. Please switch to the matching version and run again.'.format(ipex_version, torch.__version__))
  exit(127)

from . import cpu
from . import quantization
from . import nn
from . import jit

from .utils.verbose import verbose
from .frontend import optimize, enable_onednn_fusion
from .backends.cpu import set_fp32_low_precision_mode, get_fp32_low_precision_mode, LowPrecisionMode

