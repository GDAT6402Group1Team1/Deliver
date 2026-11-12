# Delivery 文档目录

先看 [职责、资源与流程总览](Architecture.md)，再读 [项目阅读路线](READING_GUIDE.md)，再按问题查专题。方案、历史与参数记录不能单独当作当前功能证明；以代码、蓝图配置和实机共同确认。

新增设计入口：[LLM 行人奇遇实施方案](Architecture.md#llm-行人奇遇实施方案2026-11-12设计阶段)，首版为“两名暴脾气 NPC 争执互殴、被撞后拽车、持续鸣笛引发抱怨/赌气躺下、玩家交涉脱身”，包含模块与状态所有权、动作/占用契约、Listen Server/AI 后端架构、W0–W6 工作包、延迟与清理、11 月 17 日迭代计划、GDC/文章/视频参考和提取状态。另含 R17–R25 工程文章/SDK 对齐、参考版本差异与 UE MCP 行为树资产交付要求。其中远期规划保留设计标记；本轮居民玩法与实际验证单独记录。

[HTML 阅读版：行人奇遇实施方案](DeliveryAIPlan.html) 按玩法、人物语音表情、架构、行为树、感知、对话、多人同步、排期和验收组织，支持离线阅读、目录跳转、精读附录展开与打印；与 Architecture.md 同步维护。

[基础设施接口与当前实现](Architecture.md#基础设施接口与当前实现2026-11-12) 记录 NPC Agent、World Gateway、动作协议、原生 BT 任务、独立 HTTP 服务的源码入口、配置、蓝图接法和验证方式。感知、正式居民 BP/行为树、交谈、互殴、拽车、字幕和卡通声线已接入；新增[居民玩法与本机 GPT](Architecture.md#居民玩法接入与本机模型2026-11-12)记录启动方法、类职责、测试证据与未验收范围。Jev 实服务、清晰人声和面部资产仍待完成。

[精读记录与方案修订](Architecture.md#精读记录与方案修订2026-11-12) 包含三场完整 GDC 字幕的回看位置、论文/代码阅读边界、动作完成条件、NPC 证据与传闻、语音指标和新增验收场景；不能将研究结果当作已实现功能。

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
