# market_fix —— 修复群星读档后市场凭空结算一次的 DLL

配合 [`stellaris_mod_injector`](https://github.com/MyresMkG/Stellaris-Mod-Injector) 使用：把编译好的 `market_fix.dll`
放进游戏根目录的 `injected_mods\`，注入器会自动把它注入 `stellaris.exe`。

**不修改任何游戏文件**，所有改动只在进程内存里，重启游戏即消失。

---

## 它解决什么问题

《群星》有一个长期存在的 bug：**读档时，市场会对挂着的月度订单空中结算一次**——
价格按这笔订单变化了，但**贸易额没扣、资源没加**。

具体表现：

- 你在市场挂了「每月买 67 能源」
- 存档 → 读档
- 读档后（游戏还暂停着）价格已经涨了，但你的能源没变、贸易额没扣

实测证据（同一局，挂单 67 能源）：

| 字段 | 挂单后存档 | 读档后立刻存档 | 变化 |
| --- | --- | --- | --- |
| `monthly_trades` | 67 | 67 | 不变（订单还在） |
| `resources_bought` 能源 | 100 | **167** | **+67**（正好是挂单量） |
| `internal_market_fluctuations` 能源 | 2 | **3.34** | +1.34 |

也就是说：**读档把挂单当成"已经买过"计进了累计买入量，据此推高了市场价，但没有执行资源转移。**

反过来同样成立：挂卖单时反复读档，可以让市场价格一路走低而不用真出货。

---

## 根因

读档时会走一遍市场对象的反序列化，**紧接着**游戏会执行一次
`FUN_1408F50A0`（月度市场更新）。这个函数本来只在月底结算时跑，它做的事是：

1. 遍历 `monthly_trades` 里的每一笔挂单
2. 把成交额计入 `resources_bought` / `resources_sold`
3. 更新 `internal_market_fluctuations`

正常情况下这一步之后还有资源转移；但读档路径上**只跑了价格部分**，于是市场空涨。

实测日志（hook 两个函数调用点）：

```
[25870921] read arm=1                      ← 读档读取市场对象
[25874718] SUPPRESS monthly-update #1      ← 紧跟的月度更新被拦下
[25902984] pass monthly-update #1          ← 之后的正常月度更新照常运行
...
=== detach: suppressed=1 passed=13 ===
```

---

## 它怎么修

**思路：读档时给一个标记，把紧跟其后的那一次月度更新吞掉。**

- 读档读取市场对象时（`0x8F196C`）→ 设置 `loadArmed` 标志
- 下一次 `FUN_1408F50A0` 被调用时（两个调用点 `0x25865E` / `0x260905`）→
  若标志已设置，**直接跳过这次调用**，并清除标志
- 正常月底没有读档读取，标志不会设置，所以**月度结算不受影响**

日志证据：一次测试中 `suppressed=1 passed=13`——只吞掉了读档那次，其余 13 次正常月度更新全部放行。

---

## 补丁点（Cygnus v4.5.2，sha256 `400df27c82ddc845...`）

| 名称 | RVA | 说明 |
| --- | --- | --- |
| 读市场对象（挂标记） | `0x8F196C` | 调用 `FUN_140308990` 的站点 |
| 月度更新调用点 A（拦截） | `0x25865E` | `call FUN_1408F50A0` |
| 月度更新调用点 B（拦截） | `0x260905` | `call FUN_1408F50A0` |

**实现方式：只改写调用点的 `E8 rel32` 四个字节**，函数体一个字节都不动。
（早期版本试过覆盖函数入口，会破坏参数 home space 导致崩溃，已放弃。）

---

## 安装步骤

### 前提

- 游戏版本 **Cygnus v4.5.2**
- 已安装 [Stellaris-Mod-Injector v1.2.0](https://github.com/MyresMkG/Stellaris-Mod-Injector/releases/tag/v1.2.0) 或更新

### 1. 找到游戏根目录

Steam 里右键《群星》→ **管理** → **浏览本地文件**，会打开类似这样的目录：

```
<你安装 Steam 的盘符>\Steam\steamapps\common\Stellaris\
```

常见例子：

```
C:\Program Files (x86)\Steam\steamapps\common\Stellaris\
D:\Steam\steamapps\common\Stellaris\
E:\Steam\steamapps\common\Stellaris\
```

这个目录里应该有 `stellaris.exe`。

### 2. 确认代理 DLL 已就位

在游戏根目录里，应该已经有一个代理 DLL（任选其一即可，通常是从注入器 v1.2.0 拿到的）：

```
stellaris.exe
version.dll          ← 或 dxgi.dll / d3d11.dll / winmm.dll / d3dcompiler_47.dll
```

**没有的话**：去 [v1.2.0 发行版](https://github.com/MyresMkG/Stellaris-Mod-Injector/releases/tag/v1.2.0)
下载一个，解压到游戏根目录（和 `stellaris.exe` 同级）。

### 3. 创建补丁目录

在游戏根目录下进入 `injected_mods`，在里面新建一个文件夹，名为 `market_fix`：

```
Stellaris\
├── stellaris.exe
├── version.dll
└── injected_mods\
    └── market_fix\          ← 新建这个文件夹
```

**必须是 `injected_mods` 的直接子目录。​** 注入器只扫描一层，根目录和更深层都不认。

### 4. 放入 DLL

把本项目的 `market_fix.dll` 复制进 `market_fix` 文件夹：

```
Stellaris\
├── stellaris.exe
├── version.dll
└── injected_mods\
    ├── achievement_unlocker\          ← 你原有的，保持不动
    │   └── achievement_unlocker.dll
    └── market_fix\
        └── market_fix.dll              ← 新增
```

### 5. 启动游戏，检查加载

正常启动游戏（双击 `stellaris.exe`，或从 Steam 启动都行）。

打开：

```
<游戏根目录>\injected_mods\stellaris_mod_injector.log
```

应该能看到：

```
2 candidate(s), 2 to load
loading [1/2] achievement_unlocker.dll ... ok
loading [2/2] market_fix.dll ... ok
2 of 2 DLL(s) loaded into pid XXXXX
```

看到 `market_fix.dll ... ok` 就是加载成功。

### 6. 确认补丁生效

读一个**挂着月度订单**的存档，然后打开：

```
<游戏根目录>\injected_mods\market_fix.log
```

应该能看到：

```
[时间戳] read arm=1 container=0x...
[时间戳] SUPPRESS monthly-update #1 (armed=1) market=0x...
```

`SUPPRESS` 出现即表示拦截成功。之后的 `pass monthly-update` 是正常月度结算，不受影响。

### 7. 验证效果（可选）

最直接的方法是对比两份存档：

1. 挂个月度订单，存档 → 记为 `A.sav`
2. 读 `A.sav`，进游戏后**什么都不做**，立刻存档 → 记为 `B.sav`
3. 用仓库里的 `tools/verify_fix.py` 对比

```
python tools/verify_fix.py A.sav B.sav
```

- 输出 `IDENTICAL` = 修复生效
- 输出 `DIFFERENT` 并列出差异 = 没生效

---

## 临时禁用

出问题时不用删文件，在 DLL 同目录建一个空文件：

```
injected_mods\market_fix\market_fix.dllnoinject
```

重启游戏即跳过加载。日志会显示 `[skip] market_fix.dll: disabled by ...`。
删掉这个空文件就恢复。

## 卸载

删掉整个文件夹即可：

```
injected_mods\market_fix\
```

重启游戏后完全移除。**游戏本体文件没有任何改动。**

## 日志

日志有两份，都在 `injected_mods\` 下：

| 文件 | 内容 |
| --- | --- |
| `stellaris_mod_injector.log` | 注入器自己的日志：加载了哪些 DLL |
| `market_fix.log` | **本补丁的日志**：拦截记录 |

`market_fix.log` 示例：

```
market_fix -- suppress bogus monthly update after save load
base=0x7FF6BC180000
[READ_BOUGHT] installed at 0x8F196C
[MONTH_1] installed at 0x25865E
[MONTH_2] installed at 0x260905
install done
[25870921] read arm=1 container=000002d0cc92eba0
[25874718] SUPPRESS monthly-update #1 (armed=1) market=000002d0c3a22c00
[25902984] pass monthly-update #1 market=000002d0c3a22c00
[25910781] pass monthly-update #2 market=000002d0c3a22c00
```

- `read arm=N` — 读档读到了市场对象，设置标志
- `SUPPRESS monthly-update #N` — 紧跟其后的错误结算被拦下
- `pass monthly-update #N` — 正常月度结算，照常放行

关游戏时末尾会有一条汇总：

```
=== detach: suppressed=1 passed=13 ===
```

`suppressed` 是拦下的次数（每次读档 1 次），`passed` 是正常放行的次数。

## 构建

需要 MinGW-w64 的 `g++`：

```
g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
    -o build/market_fix.dll src/market_fix.cpp -lkernel32
```

---

## 已知限制

- **只针对 Cygnus v4.5.2**。游戏更新后 RVA 会变，需要重新定位（三个调用点都是 `call` 指令，
  可用特征码重新搜索）。
- 标志窗口约 3.8 秒（实测）。如果某次读档没有触发月度更新，标志会残留并在下一次
  正常月度结算时被消费掉——**概率极低，但存在**。需要的话可以加超时保护。
- 这是**行为拦截**而非根因修复。真正该修的是「读档路径不该跑月度结算」，但那是引擎逻辑，
  mod 层改不了。

---

## 排查方法

如果怀疑没生效，可以对比两份存档：

1. 挂单后存档 `A.sav`
2. 读 `A.sav`，进游戏后**什么都不做**立刻存档 `B.sav`
3. 解开两个 `.sav`（本质是 zip），对比 `gamestate` 里的 `market=` 块

打补丁前 `resources_bought` 会加一次挂单量；打补丁后应该完全一致。

---

## 附：自动存档数量怎么改

群星默认只保留 **5** 个自动存档，超出后覆盖最早的。这个上限由用户设置控制，
**不在 mod 里**，所以改配置文件即可。

打开：

```
Documents\Paradox Interactive\Stellaris\settings.txt
```

在文件末尾加一行（数字自己定）：

```
max_autosaves=20
```

**注意：**

- **先关游戏再改**，游戏退出时会回写 `settings.txt`，运行时改会被覆盖
- 用记事本直接打开改、`Ctrl+S` 保存即可，别另存为带 BOM 的 UTF-8
- 改完重启游戏生效，已有的多余存档不会被删

这个键当前文件里没有，是**新增一行**，不是改数值。
