"""ZMeshMend 启动器
在 ZBrush 中通过 ZScript > Python Scripting > Load 加载本文件
"""

import os
import sys

_script_dir = os.path.dirname(os.path.abspath(__file__))
_plugin_dir = os.path.join(_script_dir, "ZMeshMend")
if _plugin_dir not in sys.path:
    sys.path.insert(0, _plugin_dir)

from ZMeshMend import main

main()
