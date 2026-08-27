# PS2DEM Importer — Unreal Engine 5.8.1

这是一个项目级、纯 Python 的 Unreal Editor 插件。它不会绑定测试项目的绝对路径。

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
- `Apply Selected Splines to Landscape`：把选中的 PS2DEM 普通 Spline 应用到专用 Landscape Edit Layer。
- `Reset Landscape Deformation Layer...`：打开 Landscape 模式并显示安全清理/重建步骤。
- `Import Terrain`：检查 R16 与 metadata、进入 Landscape 模式并显示准确导入参数。
- `Show Exchange Folder`：打开当前项目交换目录。

建筑 Shape Layer 推荐直接使用 `A_高度`、`B_高度`、`C_高度` 命名，例如 `A_7`。同名建筑可重复，Photoshop 导出器会自动追加唯一编号；旧格式 `A_01_5` 继续兼容。

建筑底面和路线控制点都直接采样交换目录中的 R16 高度数据，不依赖 Landscape Collision。

UE 5.8 官方公开流程仍要求在 Landscape 面板中确认新 Landscape 的 Import；Python 没有公开稳定的新 Landscape 创建接口。因此地形菜单完成文件验证、进入模式和参数准备，最后仍需在 Landscape 面板选择 R16 并点击 Import。这样比调用未公开接口更容易迁移到 5.8.1 正式项目。

建筑导出默认匹配 Landscape 导入时未勾选 **Flip Y Axis** 的方向。如果地形导入时主动勾选了该选项，把 `ExportBuildings.jsx` 顶部的 `FLIP_Y_FOR_UE` 改为 `true` 后重新导出建筑。

## Photoshop 路线组

PSD 中建立 `MainRoad`、`BranchRoad`、`River` 三个组。组内每个 Shape/路径图层代表一条路线，可以使用钢笔贝塞尔曲线。运行 `ExportSplines.jsx` 后会输出 `splines.json`：主路宽度 6m、支路 3.5m、河流 12m。UE 中首先生成普通可编辑 Spline Actor，不会在导入时直接修改地形。

## 将选中路线应用到 Landscape

该功能保留普通 Spline 作为规划母线，只在用户明确选择路线后修改 Landscape：

1. 进入 Landscape Mode 的 `Sculpt > Edit Layers`。
2. 新建一个**普通 Edit Layer**，严格命名为 `PS2DEM_Splines`。不要创建专用的 `Spline Edit Layer`。
3. 回到选择模式，在 World Outliner 或视口中选中需要处理的 MainRoad、BranchRoad 或 River Actor。
4. 如果关卡中有多个 Landscape，同时选中目标 Landscape；只有一个时插件会自动识别。
5. 选择 `PS2DEM → Apply Selected Splines to Landscape`，检查确认窗口后执行。
6. 检查结果，满意后再保存关卡。插件不会自动保存。

变形规则：

- MainRoad、BranchRoad：允许抬高和降低地形，形成连续路基。
- River：只允许降低地形，避免河道反向抬高地面。
- JSON 中的路线宽度表示总宽度；应用到 Landscape 时左右各使用总宽度的一半。
- 两侧平滑过渡宽度默认约等于半宽，最少 2m。
- 路线越长，插件会增加细分，但限制在 20–256，避免极端数值导致速度过慢或伪影。

`Editor Apply Spline` 是累积式地形编辑。移动路线后再次应用不会自动擦掉旧位置的变形。需要干净重建时，选择 `Reset Landscape Deformation Layer...`，然后在 Edit Layers 面板删除 `PS2DEM_Splines` 并重新创建同名普通层，再重新应用路线。

UE 5.8.1 已向 Python 暴露普通 Spline 的 Landscape 变形接口，但没有公开稳定的 Edit Layer 创建和删除接口。因此创建、删除这两个操作保留为手动步骤，插件会在目标层缺失时拒绝修改基础地形。
