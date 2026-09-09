# PS2DEM Importer — Unreal Engine 5.8.1

这是一个项目级 Unreal Editor 插件，使用 Python 处理交换文件，并用一个小型 C++ 编辑器模块创建 UE 原生 Landscape Spline。它不会绑定测试项目的绝对路径。

## 安装

把整个 `PS2DEMImporter` 文件夹复制到目标项目：

```text
<YourProject>/Plugins/PS2DEMImporter
```

最终结构必须是：

```text
<YourProject>/Plugins/PS2DEMImporter/PS2DEMImporter.uplugin
```

打开项目，在 Plugins 中启用 `PS2DEM Importer`、`Python Editor Script Plugin` 和 `Editor Scripting Utilities`，然后重启 UE。顶部菜单会出现 `PS2DEM`。

也可以在 PowerShell 中使用随包提供的安装脚本：

```powershell
.\Install-PS2DEMImporter.ps1 -ProjectPath "D:\你的项目目录"
```

安装脚本会验证目录中恰好存在一个 `.uproject`，再复制插件；同一个命令也可以用于更新插件。

## 项目交换目录

插件只读取当前项目自己的：

```text
<YourProject>/Saved/PS2DEMImporter/
├── buildings.json
├── splines.json
└── Terrain/
    ├── height_ue.r16
    ├── metadata.json
    ├── height_preview.png
    └── height_float.npy
```

因此从测试项目迁移到正式项目时不需要修改 Python 路径。只需复制插件，并让 Photoshop/DEM 工具把数据输出到正式项目的 `Saved/PS2DEMImporter`。

## 菜单

- `Import / Reimport Buildings`：自动替换 A/B/C 建筑白盒。
- `Import / Reimport Splines`：自动替换 MainRoad、BranchRoad、River 普通路线 Spline。
- `Convert / Rebuild Landscape Splines`：把选中的 PS2DEM 普通路线转换成可编辑、可保存、可挂 Mesh 的原生 Landscape Spline。
- `Remove Generated Landscape Splines...`：只删除工具生成的 Landscape Spline，不碰手工样条。
- `Assign Selected Mesh to Main Roads / Branch Roads / Rivers`：把内容浏览器中唯一选中的 Static Mesh 批量绑定给对应类型。
- `Diagnose Splines / Landscape...`：只读比较选中路线、当前 UE 地表和 R16 高度，并输出逐点 JSON 日志。
- `Import Terrain`：检查 R16 与 metadata、进入 Landscape 模式并显示准确导入参数。
- `Show Exchange Folder`：打开当前项目交换目录。

建筑 Shape Layer 推荐直接使用 `A_高度`、`B_高度`、`C_高度` 命名，例如 `A_7`。同名建筑可重复，Photoshop 导出器会自动追加唯一编号；旧格式 `A_01_5` 继续兼容。

建筑底面和路线控制点都直接采样交换目录中的 R16 高度数据，不依赖 Landscape Collision。

Photoshop 路径手柄只控制路线的平面曲率。导入器会保留控制点采样到的地形高度，但忽略手柄的 Z 差值，避免三维贝塞尔切线过冲并在 Landscape 上产生尖峰或深沟。

转换时插件沿普通路线约每 10 米检测一次当前 Landscape 地表，并生成密集的原生 Landscape Spline 控制点。这样既能贴地，又能避免两个遥远端点之间的 Z 插值穿过山体。Spline Edit Layer 的形变和路面 Mesh 都由 UE 自己管理。

UE 5.8 官方公开流程仍要求在 Landscape 面板中确认新 Landscape 的 Import；Python 没有公开稳定的新 Landscape 创建接口。因此地形菜单完成文件验证、进入模式和参数准备，最后仍需在 Landscape 面板选择 R16 并点击 Import。这样比调用未公开接口更容易迁移到 5.8.1 正式项目。

建筑导出默认匹配 Landscape 导入时未勾选 **Flip Y Axis** 的方向。如果地形导入时主动勾选了该选项，把 `ExportBuildings.jsx` 顶部的 `FLIP_Y_FOR_UE` 改为 `true` 后重新导出建筑。

## Photoshop 路线组

PSD 中建立 `MainRoad`、`BranchRoad`、`River` 三个组。组内每个 Shape/路径图层代表一条路线，可以使用钢笔贝塞尔曲线。运行 `ExportSplines.jsx` 后会输出 `splines.json`：主路宽度 6m、支路 3.5m、河流 12m。UE 中首先生成普通可编辑 Spline Actor，不会在导入时直接修改地形。

## 转换为 Landscape Spline

普通 Spline 继续作为 Photoshop 路线的规划母线。需要 UE 地形形变或道路 Mesh 时：

1. 在 World Outliner 或视口中选中需要转换的 MainRoad、BranchRoad 或 River Actor。
2. 如果关卡中有多个 Landscape，同时选中目标 Landscape；只有一个时插件会自动识别。
3. 选择 `PS2DEM → Convert / Rebuild Landscape Splines`，确认后执行。
4. 插件会自动创建真正的 Spline Edit Layer。若旧版本留下了同名的普通 `PS2DEM_Splines` 层，会先清除旧形变，再迁移为 Spline Edit Layer。
5. 进入 `Landscape Mode → Manage → Splines` 可继续移动控制点或手动调整参数。满意后再保存关卡；插件不会自动保存。

变形规则：

- MainRoad、BranchRoad：允许抬高和降低地形，形成连续路基。
- River：只允许降低地形，避免河道反向抬高地面。
- JSON 中的路线宽度表示总宽度；Landscape Spline 的 Width 使用一半宽度。
- 两侧平滑过渡宽度默认约等于半宽，最少 2m。
- 路线约每 10m 创建一个控制点，以跟随现有地表，原始普通 Spline 不会因此增加控制点。

再次执行转换时，只会替换所有名称以 `PS2DEM_` 开头的工具生成点和线段；手工 Landscape Spline 保留不变。也可以用 `Remove Generated Landscape Splines...` 单独清除生成内容。

## 给 Landscape Spline 绑定 Mesh

1. 在 Content Browser 中只选中一个道路或河流 Static Mesh。
2. 选择对应菜单：`Assign Selected Mesh to Main Roads`、`...Branch Roads` 或 `...Rivers`。
3. 插件会覆盖该类型生成线段的 Mesh 列表，并启用 `Scale to Width`。默认约定 Mesh 的长度方向是本地 X、上方向是 Z。
4. 如果模型方向不对，可在 `Landscape Mode → Manage → Splines` 中选中线段，手动调整 Forward Axis、Up Axis、缩放或偏移。

Mesh 和地形形变现在都属于 UE 原生 Landscape Spline，因此不会再出现“普通 Edit Layer 已经压地形，但 Mesh 属于另一套对象”的脱节情况。

如果应用路线后同时出现异常凸起和沟壑，先选中有问题的路线，运行 `Diagnose Splines / Landscape...`。工具每 5 米向当前 Landscape 做一次只读垂直检测，日志写入 `<项目>/Saved/PS2DEMImporter/Diagnostics`，其中包含 R16 目标高度、当前 Landscape 高度、原始 Spline 高度、Y 翻转对照、地形文件时间和编辑层列表。诊断不会修改关卡。
