# -*- coding: utf-8 -*-
"""只重导摩托车骑手（`SK_MotorbikeRider`），不动那 12 个车体静态网格。

用法：编辑器控制台 `py reimport_motorbike_rider.py`

改了只影响骨骼网格的导入选项（比如 `use_t0_as_ref_pose`）时用这个。
整套重跑会因为"车体资产已存在"直接跳过导入，骑手根本不会被重导——
第一次就是这么白跑了一轮的。

结果看 Saved/setup_motorbike.txt，关键是这两行：
    use_t0_as_ref_pose 读回 = True     ← 选项真的写进去了
    骑手高度 ...cm（坐姿应约 120，站姿应约 190）
"""

import importlib

import setup_motorbike

importlib.reload(setup_motorbike)
setup_motorbike.reimport_rider()
