# SGame ESP Zygisk — 复刻说明

基于 [wwweeeqqu/honor-of-kings-RE-research](https://github.com/wwweeeqqu/honor-of-kings-RE-research)
的 `zygisk-esp` 骨架改造。目标：读英雄坐标 + 血量，通过 overlay 显示。

---

## 一、这套东西由什么组成

```
zygisk 模块 (module/)          ← 注入游戏进程,读内存
    ↓ TCP 127.0.0.1:47291
overlay app (overlay-app/)     ← 独立悬浮窗 App,画点/血条
```

两块必须都装。模块负责取数，App 负责显示，互不干扰。

---

## 二、核心偏移（本工程使用的）

### 2.1 入口链（IL2CPP，v33 实证）

```
ActorManager.s_instance                      (静态字段, 父类上)
  +0x10  updatableActorList                  DictionaryView<UInt32, ActorConfig>
    .Context  (运行时反射取偏移)
      +0x10  _entries   (Entry[])      stride 24
      +0x18  _count     (int)
        Entry[    i]:
          +0x00  hash  (i32)
          +0x04  next  (i32)
          +0x08  key   (u32)        ← actor key
          +0x10  value (ptr)        ← ActorConfig*
            +0x18  ActorType (0 = HERO)
            +0x50  inner → ActorConfigInner*
              +0x08  actorLinker → ActorLinker*
```

### 2.2 实时数据（都挂在 ActorLinker 上）

| 项目 | 偏移 | 类型 | 备注 |
|---|---|---|---|
| **坐标** | `+0x4C4` | Vector3 (3×f32) | world units，直接可用 |
| **朝向** | `+0x4B8` | VInt3 (3×i32) | ×1000 定点，画箭头 |
| **阵营** | `+0x56C` | i32 | 1=蓝 2=红 |
| **英雄ID** | `+0x038` | i32 | 用于区分英雄 |
| **ObjID** | `+0x4AC` | u32 | |

### 2.3 血量（跨模块验证，2026-06-02 设备实测）

```
hp_anchor = *(ActorConfig + 0x188)
cur_hp    = *(int32*)(hp_anchor + 0xA8)
max_hp    = *(int32*)(hp_anchor + 0xAC)   ← 本 build 读出恒 0，已弃用

HP 与坐标不同源。baba 的公开文档把 +0x188/+0xA8 标成 "position"，是错的。
```

**死亡判定**：`cur_hp == 0` 连续 3 帧 → 视为死亡（在 App 侧做）。

---

## 三、数据包协议（56 字节/英雄）

```
偏移  类型    字段
0x00  u32     frame_id
0x04  u32     count
--- 每个英雄 56 字节 ---
0x00  u32     key
0x04  u32     type          (0=HERO)
0x08  i32     camp          (1/2)
0x0C  i32     configId
0x10  u32     objId
0x14  f32     x             (world)
0x18  f32     y
0x1C  f32     z
0x20  i32     fwd_x
0x24  i32     fwd_y
0x28  i32     fwd_z
0x2C  i32     hp
0x30  i32     maxHp
0x34  u32     battleOrder
---
0x38*count 之后: magic "ESP2" + "-" + frame_id + "\0"
```

**长度**：`8 + 56*count + 尾标`。App 按此解析。

---

## 四、编译（GitHub Actions，无需本地 NDK）

1. **Fork 或新建仓库**，把本工程推上去
2. 进入仓库 → Actions → Build → Run workflow
3. `package_name` 填 `com.tencent.tmgp.sgame`
4. 等 5–10 分钟
5. 下载两个 artifact：
   - `sgame-esp-zygisk-module` ← Magisk 模块 zip
   - `sgame-esp-overlay-apk` ← 悬浮窗 App

**注意**：Actions workflow 里 `sed` 会按你填的包名覆盖 `game.h`，所以即使你本地不填也能编。

---

## 五、装机步骤

### 5.1 装模块

1. 把 `magisk_module_release/*.zip` 传到手机（或让 Magisk 从文件安装）
2. Magisk → 模块 → 从本地安装 → 选 zip
3. **重启手机**

### 5.2 装 App

```
adb install overlay-app/build/outputs/apk/debug/app-debug.apk
```
或直接点 APK 安装。

首次启动要授两个权限：
- **悬浮窗**（会跳设置页）
- **通知**（前台服务需要）

### 5.3 启动顺序

1. 先开 App，点「启动」
2. 再进游戏
3. 回到 App 切换悬浮窗开关

---

## 六、验证（阶段③，关键）

**不要急着看画面，先看日志。**

```
adb logcat -s sgesp:V | grep "\[esp\]"
```

看模块 tag 是什么（在 `log.h` 里定义，当前应为 `sgesp`）。

**期望输出：**

```
[esp] ActorManager klass=0x... image=Assembly-CSharp
[esp] ActorManager.s_instance @ +0x... (static)
[esp] DictionaryView.Context @ +0x...
[esp] scan: dict_count=N heroes=M
[esp] hero[0] key=... cfg=... camp=1 pos=(12.3,0.0,-45.6) hp=3805/0
```

**判定标准：**

| 现象 | 结论 |
|---|---|
| 完全无 `[esp]` 输出 | 模块没加载 → 检查 Magisk 模块是否启用、重启过没 |
| 有 `ActorManager klass` 但无 `dict_count` | 入口链断 → `Context` 偏移不对，需现场调 |
| `dict_count` 有值但 `heroes=0` | `ActorType!=0` 判定错，或 inner/linker 链断 |
| `heroes>0` 但 `pos=(0,0,0)` | 坐标偏移错 |
| `pos` 有值但不动 | **FOW 冻结**（见下） |

---

## 七、已知问题（必须知道）

### 7.1 FOW（战争迷雾）坐标冻结

敌人不在视野时，`ActorLinker.position` 会冻在最后已知位置。

**这是上游项目没解决的遗留问题**（作者试到 v47，试过 Wwise 音频引擎的 `AkGameObj.m_position` 作旁路，未验证成功）。

**本工程的处理**：`read_pos_linker()` 里加了合理性过滤（±400 范围、NaN 检查），
**过滤掉垃圾值，但滤不掉"合理的旧值"**。所以你会看到迷雾里的敌人停在原地。

**如需彻底解决**，两条路：
1. 走 `SGW.GetDisplayData()`（服务端发来的全量真值，绕过迷雾）
2. 走 native 链（`truevision` 的 KPM 方案，完全绕开 IL2CPP）

### 7.2 `Player.captainLogicPos @ +0x408` 是陷阱

这个字段**只在恢复快照时写**，不是实时。别用。

### 7.3 `GamePlayerCenter.playersCache` 在本 build 可能为空

上游 v28→v32 踩过坑。本工程**不用这条路径**，走 `ActorManager`。

---

## 八、下一步

**阶段①（血量显示）** 已包含在本工程里 —— `EspActor.hp` 已在数据包里，App 侧画血条。

**阶段②（世界→屏幕投影 ESP 框）** 需要补：

```
libil2cpp.so base
  + 0x53D230  → mvp_root
      +0xB8  → +0x00 → +0x08 → +0x128   = 64B MVP (4×4 f32, 列主序)
```

然后 `screen = MVP * vec4(world, 1.0)`，透视除法，映射到屏幕像素。

**注意**：这个偏移来自 `truevision` 的 native 侧，**在当前 APK 版本是否有效没验证过**。
先跑阶段③确认坐标链通，再上投影。
