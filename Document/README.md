# Delivery 文档目录

先看 [职责、资源与流程总览](Architecture.md)，再读 [项目阅读路线](READING_GUIDE.md)，再按问题查专题。方案、历史与参数记录不能单独当作当前功能证明；以代码、蓝图配置和实机共同确认。

[AI 系统阅读版](DeliveryAIPlan.html) 从玩家发送消息开始，说明 NPC 如何感知、请求模型、执行动作，并将结果同步给其他玩家。包含 7 张类图、2 张时序图、函数说明、配置设计、延迟优化计划和问题记录。类图使用中文标题，代码名用于查找实现。

正文来自 [主文档的 AI 章节](Architecture.md#ai-居民架构参考2026-11-12)。其中分别标注已有代码与后续设计，并整理了 Epic、Convai、腾讯 PuerTS 的源码参考，以及 GDC 和论文阅读摘要。HTML 可离线阅读、放大图形和打印。

生成命令为 `python -X utf8 Tools/Documentation/build_ai_docs.py`，排版检查命令为 `python -X utf8 Tools/Documentation/check_ai_docs.py`。这些命令不会连接游戏或模型。

[历史设计](Architecture.md#llm-行人奇遇实施方案2026-11-12设计阶段) 和 [原始阅读记录](Architecture.md#精读记录与方案修订2026-11-12) 保留此前的方案、测试日志与引用范围。旧类名和旧方案按当时情况保存，当前接口以新的 AI 章节和源码为准。

| 文档 | 阅读用途 |
| --- | --- |
| [AudioSystem.md](AudioSystem.md) | 专题说明：结合代码与当前资产阅读 |
| [LightSplinePlan.md](LightSplinePlan.md) | 方案与备选：对照当前代码 |
| [MotorbikeRider.md](MotorbikeRider.md) | 专题说明：结合代码与当前资产阅读 |
| [Ragdoll.html](Ragdoll.html) | 专题说明：结合代码与当前资产阅读 |
| [READING_GUIDE.md](READING_GUIDE.md) | 先读：代码主流程 |
| [TaskSystem.md](TaskSystem.md) | 专题说明：结合代码与当前资产阅读 |
| [TrafficLightGroups.md](TrafficLightGroups.md) | 专题说明：结合代码与当前资产阅读 |
| [TrafficLights.md](TrafficLights.md) | 专题说明：结合代码与当前资产阅读 |

本次保留原文件位置，已有链接不变。
