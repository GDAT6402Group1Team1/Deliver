"""下载小芳固定音色使用的 0.6B Base，复用已验证文件，不加载模型"""
from prepare_voice_design import main


if __name__ == '__main__':
    main('voice_clone.json')
