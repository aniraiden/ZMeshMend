"""ZMeshMend 插件 - 自动加载初始化

把本文件所在目录加到 PYTHONPATH 或 ZBRUSH_PLUGIN_PATH
环境变量中，ZBrush 启动时会自动加载插件。

也可以直接在 ZScript > Python Scripting > Load 中加载 ZMeshMend.py。
"""

import os
import sys

_script_dir = os.path.dirname(os.path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

from ZMeshMend import main as _main

_main()
