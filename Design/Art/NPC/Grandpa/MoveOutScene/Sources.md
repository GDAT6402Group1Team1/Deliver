# 《汤喝完再搬》源素材

以下模型、贴图和动画都在当前目录内，可以离线导入。每个 Kenney 模型目录的 `Textures/colormap.png` 属于该套素材，导入时保留相对路径。

| 用途 | 文件 | 作者和许可 |
| --- | --- | --- |
| 手持锤子 | [tool-hammer.glb](Props/Kenney-SurvivalKit/tool-hammer.glb) | Kenney · CC0 · [Survival Kit](https://kenney.nl/assets/survival-kit) · [随包许可](Props/Kenney-SurvivalKit/License.txt) |
| 有汤的碗、空碗 | [bowl-soup.glb](Props/Kenney-FoodKit/bowl-soup.glb)、[bowl.glb](Props/Kenney-FoodKit/bowl.glb) | Kenney · CC0 · [Food Kit](https://kenney.nl/assets/food-kit) · [随包许可](Props/Kenney-FoodKit/License.txt) |
| 爷爷搬走的小木凳 | [stool.obj](Props/laleksic-WoodenStool/stool.obj)、[stool.mtl](Props/laleksic-WoodenStool/stool.mtl)，保留同目录两张木纹贴图 | laleksic · CC0 · [作者发布页](https://opengameart.org/content/wooden-stool) |
| 举物姿势、交接、从椅子起身和说话 | [Quaternius_UAL1_GrandpaScene.glb](Animations/Quaternius_UAL1_GrandpaScene.glb) | Quaternius · CC0 · [Universal Animation Library](https://quaternius.com/packs/universalanimationlibrary.html) · [随包许可](Animations/Quaternius_License.txt) |
| 单手提物、持物行走、喝汤和叉手反应 | [Quaternius_UAL2_GrandpaScene.glb](Animations/Quaternius_UAL2_GrandpaScene.glb) | Quaternius · CC0 · [Universal Animation Library 2](https://quaternius.com/packs/universalanimationlibrary2.html) · [随包许可](Animations/Quaternius2_License.txt) |
| 弯腰捡起地上的凳子 | [CMU_26_09_PickupFloor_SourcePreview_ZUp.fbx](Animations/CMU_26_09_PickupFloor_SourcePreview_ZUp.fbx) | Carnegie Mellon University Graphics Lab · [官方使用条款](https://mocap.cs.cmu.edu/) · [科目 26 原动作](https://mocap.cs.cmu.edu/search.php?subjectnumber=26) |

Kenney 的锤子、碗和 laleksic 凳子保留作者的原模型与贴图。两个 Quaternius GLB 从仓库现有标准版筛出所需动作，保留原骨架、模型、插值和动画采样。两份 GLB 都带 `A_TPose`，可用于重定向姿势参考。

Blender 5.0.1 已独立读回四个道具模型和三份动画文件，核对贴图、骨架和动作名。UE 导入、人物重定向和游戏内挂点随接入验收。

- 举锤可从 `Idle_Torch_Loop` 和 `Idle_Lantern_Loop` 里选持物姿势，配合姿态过渡。片段自身是循环持物动作。
- 端碗和交接可以用 `Interact`、`PickUp_Table`，以及 `Walk_Carry_Loop` 的持物姿势。`Consume` 是喝汤候选。藏到身后的动作需要在 UE 中编排，结合碗挂点和手部 IK 调整。
- 拿凳子时按高度选择 `PickUp_Table` 或 CMU 弯腰片段，站起后衔接 `Walk_Carry_Loop`。CMU 源片段为 120 Hz、3.30 秒，手部最低位置约在 1.75 秒。预览 FBX 带源骨架和蒙皮供导入使用，正式角色需要重定向。
- 大壮出面可复用 `Idle_Talking_Loop`、`Idle_FoldArms_Loop` 和 `Interact`，接碗后用 `Walk_Carry_Loop`。爷爷受惊另有 `Hit_Knockback` 候选，正式动画仍按实机观感选用。

CMU FBX 来自官方 26_09 动作的原始第 73–469 帧，沿用项目此前转换的 Z-up 版本。CMU 允许在商业成品中使用动作，禁止直接转售数据及其转换版本。发布使用该数据的研究成果时，采用 CMU 官网给出的致谢。

> The data used in this project was obtained from mocap.cs.cmu.edu. The database was created with funding from NSF EIA-0196217.
