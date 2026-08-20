# Delivery

欢乐向奇遇送快递游戏。玩家在风格各异的地区当快递员，完成配送的同时，也可以对沿途的人和事物搞点小恶作剧。

引擎：**Unreal Engine 5.8**

你是一个快递员。地图会换地区、换气质，任务也不只是“送到就结束”：在把东西交到客户手上之前，沿路可以捉弄 NPC、和场景里的物件互动。认真送货能赚钱升级；一路作妖则构成这款游戏的欢乐奇遇感。

- **单人**：独自完成任务、升级、推进流程。
- **多人**：互相比拼。谁先把物品交到客户手上，谁拿走该任务奖励。

## Git 操作

二进制资源（`.uasset` / `.umap` 等）走 **Git LFS**。克隆前请安装 [Git LFS](https://git-lfs.com)。

### 克隆

```bash
git lfs install
git clone <仓库地址>
cd Delivery
git lfs pull
```

### 日常同步

```bash
git pull
git lfs pull
```

提交前确认没有把可生成目录加进去。下列路径已被 `.gitignore` 忽略，不要强制添加：

- `Binaries/`
- `Intermediate/`
- `Saved/`
- `DerivedDataCache/`
- `*.sln` / `.vs/` 等 IDE 工程文件

```bash
git add -A
git status
git commit -m "简短说明这次改了什么"
git push
```

### 打开工程

用 Unreal Engine 5.8 打开 `Delivery.uproject`。需要 Visual Studio 工程时，右键 `.uproject` → **Generate Visual Studio project files**，在本地生成 `.sln`，不要提交。

### 许可

见 [LICENSE](LICENSE)（MIT）。
