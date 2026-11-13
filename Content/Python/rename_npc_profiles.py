"""兼容旧改名入口，使用当前人物配置与资源检查"""
from pathlib import Path
import runpy


def main():
    # 姓名、人物介绍和保存检查统一由当前脚本处理
    runpy.run_path(str(Path(__file__).with_name('update_npc_personas.py')), run_name='__main__')


if __name__ == '__main__':
    main()
