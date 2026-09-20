# -*- coding: utf-8 -*-
"""提前停掉 monitor_car.py 挂上的每帧回调并落盘。

回调句柄存在 unreal 模块对象上（py script.py 每次都是全新的 __main__，
模块级全局留不住，所以只能挂在一个跨脚本可见的地方）。

用法：py stop_monitor.py
"""

import traceback

import unreal

KEY = "_delivery_car_monitor"

try:
    st = getattr(unreal, KEY, None)
    if st is None:
        unreal.log_warning("[carmon] 没有在跑的监控")
    else:
        if st.get("handle") is not None:
            unreal.unregister_slate_post_tick_callback(st["handle"])
        out = unreal.Paths.project_saved_dir() + "car_monitor.txt"
        rows = st.get("rows", [])
        lines = []
        for n in st.get("notes", []):
            lines.append(n)
        lines.append("")
        lines.append("%-7s %-26s %8s %6s %-7s %-7s %6s %7s   %s" % (
            "时间", "位置", "车速", "样条", "红灯", "停哪", "避让", "前车",
            "我的探测：前方第一个 Inter_"))
        lines.append("-" * 150)
        lines.extend(rows)
        lines.append("")
        lines.append("手动停止，共 %d 行。" % len(rows))
        with open(out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        delattr(unreal, KEY)
        unreal.log("[carmon] 已停止，%d 行写入 %s" % (len(rows), out))
except Exception:
    unreal.log_error("[carmon] stop fail:" + chr(10) + traceback.format_exc())
