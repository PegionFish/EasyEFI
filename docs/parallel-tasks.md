# EasyEFI 并行开发任务书（subagent 派单规格）

- 日期：2026-11-22（design.md rev3 配套）
- 用法：**每个 Sxx 任务包可直接作为一个 subagent 的任务 prompt 主体**。派单时附上：本任务包全文 + `docs/design.md` 对应章节 + Wave 0 产出的契约头。
- 前提：Wave 0（S00）完成并验收后才允许批量派发 Wave 1。所有依赖只指向**契约（含 stub/mock）**，不指向其它流的实现——17 个流可同时开工。
- 契约权威：本文的接口签名是契约规格，S00 落地为 `Include/Contract/*.h` 后以头文件为准；实现流**不得擅改契约**，变更走 §4 流程。

## 0. 全局规则（所有流适用）

1. **独占所有权**：只写自己的目录（见各包）+ `docs/handoffs/Sxx.md`。leader 独占文件清单见 design.md §11.7，碰都不要碰。
2. **分支/worktree**：每流一个 git worktree，分支 `stream/Sxx-<name>`；逻辑提交（可独立 revert），不混流提交。
3. **纯逻辑纪律**：标注"纯逻辑"的库禁止 include MdePkg/UEFI 协议头；只用 `Contract/EfiCompat.h`（宿主兼容类型）+ `Contract/EfiMem.h`（MemAlloc/MemFree/MemRealloc，UEFI=AllocatePool 系，宿主=malloc 系）。**CHAR16 字面量一律 `U16("...")` 宏，字符串操作用 EfiStr* 助手，禁止裸 L"" 与 wcs*（宿主 wchar_t 32 位，S00 自检实证）**。
3b. **构建适配（S00 实证，详见 docs/build-recipe.md）**：工具链标签 `-t GCC`（锁定 edk2 已移除 GCC5）；前缀 env：X64=`GCC_BIN`、AA64=`GCC_AARCH64_PREFIX`；容器内 `CONF_PATH=/work/Conf` 且预创建；worktree 构建传 `EDK2_DIR` 指向主仓库共享 edk2。上游文档的 GCC5 写法一律替换。
4. **core/glue 纪律**：应用 core 只依赖 Ops 表（TuiOps/FsOps/ExecOps），禁止直接调 gBS/gST；glue 负责装配后端与入口。
5. **验证义务**：宿主测试（编号用例全绿）+ QEMU 冒烟（serial 断言脚本随交付）是 DoD 硬条件，不是可选项。
6. **范围冻结**：design.md D16 MVP 范围适用所有流；超范围想法写进交接注记的"建议"节，由 leader 裁决，不擅自实现。
7. **配置优先**：阈值/行为参数走 PCD 或契约常量表，不硬编码（用户偏好）。
8. **交接注记** `docs/handoffs/Sxx.md` 固定模板：
   ```
   # Sxx 交接
   ## 交付清单（文件列表）
   ## 测试证据（宿主用例编号→结果；冒烟断言脚本路径+日志摘要）
   ## 契约符合性（无变更 / 变更提案编号）
   ## 已知限制
   ## 集成注意事项（给 leader）
   ## 超范围建议（可选）
   ```

## 1. 契约总览（S00 落地，全部流只读）

| 契约头 | 内容 | 实现流 | 消费流 |
|---|---|---|---|
| EfiCompat.h / EfiMem.h | 宿主兼容类型 + 内存抽象 | S00 | 全部纯逻辑/core |
| TuiTheme.h | 颜色属性/键位命令枚举/占位符常量（冻结常量表） | S00 冻结 | S06–S13 |
| TuiScreen.h | 屏幕缓冲 Ops | S06 | S08/S11/S12/S13 |
| TuiInput.h | 按键 Ops + 命令映射 | S07 | S08/S11/S12/S13 |
| TuiWidget.h | 列表/对话框/输入行/横幅/键位条 | S08 | S11/S12/S13 |
| FsOps.h | 文件操作 vtable（UEFI/POSIX 双后端） | S09 | S11/S12/S13 |
| ExecOps.h | exec_efi/exec_nsh/boot_next/reset/auto 候选 | S10 | S11/S12 |
| ExecPath.h | 路径文法解析 | S04 | S03/S10/S11 |
| VolumeModel.h | GPT 设备路径匹配/P3 判定（纯逻辑） | S05 | S09/S10 |
| Utf8.h | UTF-8 token 编解码 | S01 | S02/S13 |
| EditorModel.h | 行数组编辑模型 | S02 | S13 |
| ConfigSchema.h | launcher.json 校验核心 + 抽象 JSON DOM | S03 | S11 |
| JsonDom.h | 最小 JSON DOM + 双适配器接口（EDK2 JsonLib / 宿主 jansson） | S00 定义，S03 实现适配器 | S03/S11 |
| 数据契约 | launcher.json schema（design §6）、.easyefi 标记格式、分区布局（design §3.1）、设备路径匹配规则（design §5.2） | S00 冻结 | S03/S05/S09/S15/S16 |

**通用约定**：callee 分配、caller 经 MemFree 释放；所有返回 EFI_STATUS（宿主兼容定义）；字符串参数 UEFI 侧 UCS-2、纯逻辑侧 UTF-8 token/CHAR8，边界转换在 glue。

---

## 2. Wave 0 — 地基（串行，leader 或 1 个 agent）

### S00 — 平台与契约（规模 L，所有流的前置）

**使命**：交付让 17 个流能立即并行开工的全部基础设施，并实证隔离配方。

**独占目录**：仓库骨架、`Include/Contract/`、`tests/host/`、`scripts/`、`EasyEfiPkg.dsc/.dec`（聚合器）、`edk2` submodule 指针、`docs/handoffs/` 模板、`boot/TestLauncherStub/`。

**交付物**：
1. 仓库骨架 + edk2 submodule（锁定 edk2-stable 标签，记录于 `docs/pins/S00-edk2.md`；pin 文件按流独占，leader 汇总）+ BaseTools 一次性编译。
2. **全部契约头**（§1 清单，签名以 design.md §7.1 + 本文各流接口节为准）+ 每个契约的 **stub/mock 实现**：
   - TuiOps mock：字符矩阵屏幕 + 脚本化按键队列；
   - FsOps POSIX 后端（真实临时目录）+ mock 记录器；
   - ExecOps mock 记录器（记录调用与 LoadOptions 构造）；
   - 纯逻辑库 stub（返回固定数据，供消费流先编译跑通）；
   - `TestLauncherStub.efi`（双架构：打印自身架构标记 + 置 `EASYEFI_RAN=yes` + 返回，供 S15 冒烟）。
3. `tests/host/` 宿主测试框架：compat 头、断言宏、每模块 Makefile 模板、`make test-host` 入口；jansson 宿主构建 + JsonDom 宿主适配器骨架。
4. `scripts/build-module.sh`（worktree 内按模块构建）、`scripts/mktestimage.sh --variant fat|ntfs --apps <list> [--size MB]`（最小测试镜像）、`scripts/qemu-run.sh --arch x64|aa64 --image <img> [--keys <qmp键序列>] --expect <断言脚本>`（OVMF 分体 pflash / QEMU_EFI.fd，USB 挂盘，serial 日志断言，QMP send-key 注入）。
5. **worktree 隔离配方**（写入 `docs/build-recipe.md`）：
   ```bash
   # 每流：
   git worktree add ../wt-Sxx -b stream/Sxx-name
   cd ../wt-Sxx
   export WORKSPACE=$PWD
   export PACKAGES_PATH=<共享edk2绝对路径>:$PWD
   export EDK_TOOLS_PATH=<共享edk2>/BaseTools   # S00 已编译
   source <共享edk2>/edksetup.sh --reconfig    # 生成本 worktree 独立 Conf
   bash scripts/build-module.sh <module.inf> X64
   ```
6. 聚合器机制：`EasyEfiPkg.dsc` 用 `!include` 收纳各模块 `module.dsc.inc` 碎片；.dec 同理。交付一个 dummy 模块示例（目录+碎片+宿主测试+冒烟）。
7. `config/launcher.example.json`（design §6 样例，作为 S03 黄金夹具，leader 维护）。

**验收（硬出口标准，全部实证）**：
- [ ] 两个 dummy 模块在**两个 worktree 同时构建**成功（X64），产物互不干扰；
- [ ] 宿主框架跑通 dummy 纯逻辑测试；
- [ ] mktestimage fat 变体 + qemu-run 启动到 Shell 提示符（serial 断言）；
- [ ] 全部 stub/mock 可编译，dummy 消费流链接跑通；
- [ ] ntfs 变体镜像可生成（驱动占位可用 S14 前置产物或暂缺省，注明）。

**禁止**：实现任何真实业务模块（只给 stub）；替各流写实现。

---

## 3. Wave 1 — 17 个并行流

> 每包结构：使命 / 独占目录 / 输入 / 依赖 / 交付物 / 接口规格 / 宿主测试用例 / QEMU 冒烟 / DoD / 禁止。
> 宿主测试用例编号 Txx-n 是 DoD 的客观验收依据；冒烟断言脚本（expect-*.sh）随交付。

### S01 — Utf8Lib（纯逻辑，规模 S）

**使命**：UTF-8 ↔ token 编解码，保证**任意字节序列往返恒等**（编辑器不损坏中文注释的根基）。
**独占目录**：`EasyEfiPkg/Library/Utf8Lib/`
**输入**：design §7.3 编码策略；Contract/Utf8.h、EfiCompat.h、EfiMem.h。
**依赖**：无（最底层，可第一个完成）。
**交付物**：Utf8.c/.inf/module.dsc.inc、宿主测试、交接注记。
**接口规格**（Contract/Utf8.h）：
```c
typedef enum { UTF8_TOKEN_CP, UTF8_TOKEN_RAW } UTF8_TOKEN_TYPE;
typedef struct { UINT32 Cp; UINT8 Raw; UTF8_TOKEN_TYPE Type; } UTF8_TOKEN;
EFI_STATUS Utf8Decode (CONST UINT8 *Bytes, UINTN Len, UTF8_TOKEN **Out, UINTN *Count);
EFI_STATUS Utf8Encode (CONST UTF8_TOKEN *Toks, UINTN Count, UINT8 **Out, UINTN *Len);
BOOLEAN    Utf8HasBom (CONST UINT8 *Bytes, UINTN Len);
UINTN      Utf8TokenWidth (CONST UTF8_TOKEN *Tok);   // 显示宽度：恒 1（占位对齐）
```
规则：非法序列（续字节缺失/overlong/超 U+10FFFF/0xFF 等）逐字节降级为 RAW token；BOM 解码为独立标记由上层处置；解码不丢任何字节信息。
**宿主测试**：
- T01-1 ASCII 往返；T01-2 中文（U+4E2D/U+6587…）往返逐字节相等；T01-3 emoji（4 字节）往返；
- T01-4 非法序列（截断 3 字节、overlong、0xFF、孤立续字节）→ RAW 保留且往返恒等；
- T01-5 BOM 检测（有/无/仅 BOM）；T01-6 空输入；T01-7 混合合法+非法；
- T01-8 **性质测试：随机字节 fuzz（≥10000 轮）Encode(Decode(x)) == x**；
- T01-9 宽度恒 1；T01-10 内存释放配对（valgrind/ASan 干净）。
**QEMU 冒烟**：无（纯逻辑；集成阶段由 S13 覆盖）。
**DoD**：T01 全绿 + ASan 干净 + 契约零变更 + 交接注记。
**禁止**：引入 UEFI 头；做"规范化"（NFC 等）——往返恒等优先于一切。

### S02 — EditorModelLib（纯逻辑，规模 M）

**使命**：行数组编辑模型——加载/保存/光标/插入删除/撤销分组/查找/跳行/渲染投影。
**独占目录**：`EasyEfiPkg/Library/EditorModelLib/`
**输入**：design §7.3；Contract/EditorModel.h、Utf8.h、TuiTheme.h（占位符/属性常量）。
**依赖**：S01 契约（stub 可先行，集成时换真实）。
**交付物**：模型实现 + .inf/碎片 + 宿主测试 + 交接。
**接口规格**（Contract/EditorModel.h，opaque EDITOR_MODEL）：
```c
EDITOR_MODEL *ModelNew (UINTN MaxBytes, UINTN UndoDepth);       // PCD 值由 glue 传入
EFI_STATUS ModelLoad (EDITOR_MODEL*, CONST UINT8 *Bytes, UINTN Len);  // 超限返回 EFI_BAD_BUFFER_SIZE
EFI_STATUS ModelSave (EDITOR_MODEL*, UINT8 **Out, UINTN *Len);        // 往返保留
EFI_STATUS ModelInsert (EDITOR_MODEL*, CONST UINT32 *Cps, UINTN N);   // 连续插入合并为一个撤销步
EFI_STATUS ModelBackspace (EDITOR_MODEL*);  EFI_STATUS ModelDeleteFwd (EDITOR_MODEL*);
EFI_STATUS ModelNewline (EDITOR_MODEL*);    // 新行终止符随文件主约定（CRLF/LF）
EFI_STATUS ModelMove (EDITOR_MODEL*, INTN Dx, INTN Dy);   // 含 Home/End/PgUp/PgDn 语义由 core 组合
EFI_STATUS ModelGotoLine (EDITOR_MODEL*, UINTN Line);
EFI_STATUS ModelFind (EDITOR_MODEL*, CONST UINT32 *Needle, UINTN N, BOOLEAN CaseSens,
                      MODEL_HIT **Hits, UINTN *Count);      // token 级子串
EFI_STATUS ModelUndo (EDITOR_MODEL*);
BOOLEAN    ModelModified (EDITOR_MODEL*);
EFI_STATUS ModelRenderRow (EDITOR_MODEL*, UINTN Line, UINTN ColStart, UINTN Width,
                           TUI_CELL *Out);   // ASCII 直显；非 ASCII → '?' + ATTR_NONASCII
// 元信息：行数/当前行列/BOM 有无/主终止符/编码标记（状态栏用）
EFI_STATUS ModelInfo (EDITOR_MODEL*, MODEL_INFO *Out);
```
规则：每行保留原始终止符（混合行尾文件保存后逐行不变）；BOM 有则保留不添加；撤销栈有界（UndoDepth），连续插入合并、换行/删除独立成步；撤销回保存点 → Modified=FALSE。
**宿主测试**：
- T02-1 加载/保存往返：中文注释 .nsh、BOM 文件、CRLF/LF/混合行尾、无尾换行；
- T02-2 边界编辑：文件头/尾、行首/行尾插入删除；空文件；单行无换行；
- T02-3 换行约定：CRLF 文件中新行用 CRLF，LF 文件用 LF；
- T02-4 撤销分组：连续输入=1 步；换行/删除独立；撤销到保存点 Modified 消失；栈满丢弃最旧；
- T02-5 查找：命中数/大小写开关/中文 needle/空 needle/无命中；
- T02-6 跳行与光标钳制（越界不崩）；T02-7 超限拒绝（MaxBytes+1）；
- T02-8 渲染投影：非 ASCII 占位+属性、列窗口切片、宽字符不越界；
- T02-9 fuzz：随机编辑序列后 Save→Load→Save 幂等；T02-10 ASan 干净。
**QEMU 冒烟**：无（S13 覆盖）。
**DoD**：T02 全绿 + 契约零变更 + 交接。
**禁止**：文件 I/O（模型只收发字节缓冲）；替换/多文件/语法高亮（MVP 外）。

### S03 — ConfigCoreLib + JSON 适配器（纯逻辑+薄胶水，规模 M）

**使命**：launcher.json 的 DOM 适配（双后端）+ schema 校验核心 + 错误定位。
**独占目录**：`EasyEfiPkg/Library/ConfigCoreLib/`
**输入**：design §6（schema 权威）、§5.3 容错；Contract/ConfigSchema.h、JsonDom.h、ExecPath.h；`config/launcher.example.json`（黄金夹具，只读）。
**依赖**：S04 契约（路径校验委托）、JsonDom 适配器接口（S00）。
**交付物**：校验核心 + EDK2 JsonLib→DOM 适配器（glue）+ 宿主 jansson→DOM 适配器 + .inf/碎片 + 宿主测试 + 交接。
**接口规格**：
```c
// JsonDom.h（S00 定义）：JSON_NODE {type, object/array/string/int/bool/null}
EFI_STATUS JsonDomFromEdk2 (EDKII_JSON_VALUE Root, JSON_NODE **Out);   // glue，QEMU 测
EFI_STATUS JsonDomFromHost (CONST CHAR8 *Text, JSON_NODE **Out);       // 宿主适配器
// ConfigSchema.h：
typedef struct { CHAR8 Id[64]; CHAR8 Name[41]; CHAR8 Desc[77]; UINT8 ArchMask;
                 UINT8 Action; CHAR8 Path[256]; CHAR8 Options[256];
                 BOOLEAN Danger; BOOLEAN Confirm; } CONFIG_ITEM;
typedef struct { CHAR8 Title[41]; CHAR8 Version[32]; UINTN SkipTimeout;
                 CONFIG_ITEM *Items; UINTN Count; } CONFIG;
typedef struct { UINTN ItemIndex; CHAR8 Field[32]; UINT32 Code; CHAR8 Msg[128]; } CONFIG_ERROR;
EFI_STATUS ConfigValidate (CONST JSON_NODE *Root, CONFIG **Out,
                           CONFIG_ERROR **Errs, UINTN *ErrCount,
                           CONFIG_ERROR **Warns, UINTN *WarnCount);
```
规则（逐条对应 design §6）：必填/长度/ASCII 校验；arch 枚举仅 x64/aarch64（"arm64" 报错并提示正确值）；action 枚举；整数 only（浮点报错）；id 唯一；path 委托 PathParse；danger 隐含 confirm；skip_timeout 0–30 越界钳制+警告；未知字段 → 警告不报错；非法项跳过但保留错误清单。
**宿主测试**：
- T03-1 黄金样例解析成功且字段逐一相等；T03-2..12 每类非法输入各一用例（缺 id/非 ASCII name/超长/坏 arch/浮点/重复 id/坏 path 前缀/未知 action/skip 越界/空 items/根非 object）→ 断言具体错误码与 ItemIndex；
- T03-13 未知字段 → 警告且解析成功；T03-14 danger=true 自动 confirm；
- T03-15 宿主 jansson 适配器与 EDK2 适配器对同一 JSON 产出同构 DOM（黄金对比，EDK2 侧在 QEMU 冒烟跑）；
- T03-16 大配置（200 项）性能与内存；T03-17 ASan。
**QEMU 冒烟**：fat 变体 + 测试应用：读 P3/FAT 上的 launcher.json → JsonDomFromEdk2 → ConfigValidate → serial 打印校验结果标记（好/坏样例各一）。
**DoD**：T03 全绿 + 冒烟断言 + 契约零变更 + 交接。
**禁止**：改 schema（schema 变更 = 数据契约变更，走 leader）；在核心里调 JsonLib（核心只认 DOM）。

### S04 — ExecPathLib（纯逻辑，规模 S）

**使命**：路径文法解析/规范化/架构过滤——所有 `path` 字段的唯一解释器。
**独占目录**：`EasyEfiPkg/Library/ExecPathLib/`
**输入**：design §6 路径文法、§5.6；Contract/ExecPath.h。
**依赖**：无。
**接口规格**：
```c
typedef enum { PATH_DATA, PATH_AUTO, PATH_FS, PATH_BOOT } PATH_KIND;
typedef struct { PATH_KIND Kind; CHAR16 Rel[256]; UINTN FsIndex;
                 UINT16 BootNumber; BOOLEAN BootMenu; BOOLEAN IsScript; } RESOLVED_PATH;
EFI_STATUS PathParse (CONST CHAR8 *Utf8, RESOLVED_PATH *Out);
BOOLEAN    PathArchMatch (UINT8 ItemArchMask, UINT8 CurrentArch);  // bit0=x64 bit1=aarch64
```
规则：前缀 data:/auto:/fsN:/boot:；反斜杠→正斜杠；拒绝 `..` 穿越、空路径、未知前缀、fs 索引非数字、boot 非 4 位十六进制（menu 除外）；`.nsh` 后缀 → IsScript；boot:menu → BootMenu。
**宿主测试**：T04-1 四前缀正例；T04-2 斜杠规范化；T04-3 fs 索引/boot 十六进制解析（0001/FFFF/menu）；T04-4..9 拒绝用例（空/未知前缀/`..`/fsX/boot:GGGG/超长）；T04-10 IsScript 判定；T04-11 架构掩码过滤全组合；T04-12 fuzz 不崩。
**QEMU 冒烟**：无（S10/S11 覆盖）。
**DoD**：T04 全绿 + 交接。**禁止**：运行时卷解析（那是 S09/S10 的事）。

### S05 — VolumeModelLib（纯逻辑，规模 M）

**使命**：设备路径字节缓冲上的 GPT 解析/启动盘身份/P3 判定/auto: 排除——不碰真实固件也能测。
**独占目录**：`EasyEfiPkg/Library/VolumeModelLib/`
**输入**：design §5.2（P3 发现算法）、§5.6（auto: 排除）；Contract/VolumeModel.h；UEFI Spec 2.10 §10（设备路径/GPT 节点格式，S00 提供摘录）。
**依赖**：无。
**接口规格**：
```c
typedef struct { UINT8 DiskSignature[16]; UINT32 PartNumber; UINT8 PartTypeGuid[16];
                 UINT64 PartStartLba; UINT64 PartEndLba; } GPT_NODE_INFO;
EFI_STATUS DpFindGpt (CONST UINT8 *Dp, UINTN Len, GPT_NODE_INFO *Out);      // 首个 HD(GPT) 节点
EFI_STATUS DpDiskPrefix (CONST UINT8 *Dp, UINTN Len, UINT8 **Prefix, UINTN *Len); // HD 节点之前的前缀
BOOLEAN    VolIsP3 (CONST UINT8 *BootDp, UINTN BootLen, CONST UINT8 *VolDp, UINTN VolLen);
BOOLEAN    VolSameDisk (CONST UINT8 *A, UINTN ALen, CONST UINT8 *B, UINTN BLen);
```
规则：VolIsP3 = 同盘前缀 + PartNumber==3；VolSameDisk = 前缀逐字节相等（不依赖签名 alone，前缀含控制器路径）；MBR HD 节点 → 明确拒绝（本镜像 GPT-only）；畸形/截断缓冲 → 错误返回不越界。
**宿主测试**：T05-1 合成设备路径（ACPI/PCI/USB/HD(GPT) 手工字节构造）解析正确；T05-2 嵌套分区取首个 HD；T05-3 同盘/异盘判定（含克隆盘同签名不同控制器路径）；T05-4 P3 判定（分区号 3/非 3/异盘）；T05-5 MBR 拒绝；T05-6 截断/畸形 fuzz ≥5000 轮不崩；T05-7 字节序/对齐；T05-8 ASan。
**QEMU 冒烟**：无（S09 覆盖）。
**DoD**：T05 全绿 + 交接。**禁止**：调 gBS（纯逻辑）；处理 MBR 启动（范围外）。

### S06 — TuiScreenLib（UEFI 胶水，规模 M）

**使命**：ConOut 文本模式管理 + 单元格缓冲 + 脏区重绘；TuiOps 屏幕真实后端。
**独占目录**：`EasyEfiPkg/Library/TuiScreenLib/`
**输入**：design §5.5；Contract/TuiScreen.h、TuiTheme.h。
**依赖**：TuiTheme 常量（S00 冻结）。
**接口规格**：见 design §7.1 TuiOps；实现 TuiInit/TuiGetSize/TuiPutCell/TuiPutText/TuiFill/TuiFlush/TuiClear/TuiShutdown（签名以契约头为准）。
规则：QueryMode 枚举取最大可用文本模式（保底 80×25）；缓冲=CHAR16+attr 矩阵；Flush 只重绘脏单元格（ConOut SetCursorPosition+OutputString）；Shutdown 恢复原模式；纯 ASCII 画框字符集。
**宿主测试**（mock ConOut 记录调用）：T06-1 模式选择（模拟 3 种模式集）；T06-2 脏区计算（改 1 格只输出 1 格）；T06-3 边界写入钳制；T06-4 缓冲快照黄金对比；T06-5 恢复模式调用。
**QEMU 冒烟**：fat 变体 + 测试应用：TuiInit→画框+文本→Flush→serial 标记 `[S06] mode=<cols>x<rows> PASS`；截图人工抽查（集成阶段）。
**DoD**：T06 全绿 + 冒烟 + 交接。**禁止**：GOP；CP437 制表符（用 ASCII +-|）；在库里读按键（S07 职责）。

### S07 — TuiInputLib（UEFI 胶水，规模 S）

**使命**：ConIn 按键读取 + 扫描码→抽象命令映射；TuiOps 输入真实后端。
**独占目录**：`EasyEfiPkg/Library/TuiInputLib/`
**输入**：design §5.5 键位、§7.2 F 键表；Contract/TuiInput.h、TuiTheme.h（命令枚举）。
**接口规格**：TuiWaitKey（事件等待）/TuiPollKey（非阻塞）/TuiKeyToCommand；TUI_KEY{ScanCode,UnicodeChar}；命令枚举 CMD_UP/DOWN/LEFT/RIGHT/PGUP/PGDN/HOME/END/ENTER/ESC/BACKSPACE/DELETE/F1..F10/QUIT/CHAR。
**宿主测试**：T07-1 扫描码映射全表（方向/功能键/ESC）；T07-2 可打印字符透传；T07-3 未知扫描码→CMD_NONE 不崩；T07-4 命令映射与 TuiTheme 键位表一致。
**QEMU 冒烟**：qemu-run --keys 注入（QMP send-key）方向键+Enter+F4+ESC → 测试应用 serial 打印命令序列断言。
**DoD**：T07 全绿 + 冒烟 + 交接。**禁止**：鼠标/SimplePointer（全键盘决策）；在库里做业务分发。

### S08 — TuiWidgetLib（控件，规模 M）

**使命**：列表/确认对话框/单行输入/横幅/键位条/ASCII 画框——三应用共用控件，mock 屏幕可宿主测。
**独占目录**：`EasyEfiPkg/Library/TuiWidgetLib/`
**输入**：design §5.5/§7.2/§7.3 交互；Contract/TuiWidget.h、TuiScreen.h、TuiInput.h、TuiTheme.h。
**依赖**：S06/S07 契约（mock 先行）。
**接口规格**（契约头细化）：
```c
WIDGET_LIST *ListNew (UINTN Rows);  // 可视行数
EFI_STATUS ListSetItems (WIDGET_LIST*, CONST LIST_ITEM *Items, UINTN N); // {text,attr,disabled}
EFI_STATUS ListHandleKey (WIDGET_LIST*, CONST TUI_KEY*);   // 上下/翻页/跳过 disabled/数字直选
UINTN ListSelected (WIDGET_LIST*);  VOID ListDraw (WIDGET_LIST*, UINTN Row, UINTN Col, UINTN W);
EFI_STATUS DialogConfirm (CONST CHAR16 *Title, CONST CHAR16 *Body, BOOLEAN DefaultYes, BOOLEAN *Yes);
WIDGET_INPUT *InputNew (UINTN MaxLen, BOOLEAN AsciiOnly);
EFI_STATUS InputHandleKey (WIDGET_INPUT*, CONST TUI_KEY*);  CONST CHAR16 *InputText (WIDGET_INPUT*);
VOID BannerDraw (UINTN Row, CONST CHAR16 *Text, UINT8 Attr);
VOID KeybarDraw (CONST CHAR16 *Text);   // 底部常驻
VOID BoxDraw (UINTN Row, UINTN Col, UINTN W, UINTN H, CONST CHAR16 *Title);
```
规则：danger 对话框默认 N；列表选中高亮=ATTR_SELECTED、disabled=ATTR_DISABLED 且跳过；输入行 AsciiOnly 拒绝非 ASCII 并闪提示。
**宿主测试**（mock 屏幕字符矩阵断言）：T08-1 列表滚动/视口/首尾钳制；T08-2 disabled 跳过+数字直选；T08-3 对话框 Y/N/默认值/ESC=取消；T08-4 输入行编辑/退格/上限/ASCII 过滤；T08-5 画框+键位条黄金快照；T08-6 小屏 80×25 布局不溢出。
**QEMU 冒烟**：fat 变体：渲染列表+对话框，QMP 按键操作，serial 标记选择结果。
**DoD**：T08 全绿 + 冒烟 + 交接。**禁止**：业务语义（控件不知"固件""危险"，只认 attr/flag）。

### S09 — ExecVolumeLib + FsOps 后端（UEFI 胶水+POSIX，规模 L）

**使命**：FsOps 双后端 + 卷枚举 + P3 发现（design §5.2 算法的真实实现）。
**独占目录**：`EasyEfiPkg/Library/ExecVolumeLib/`
**输入**：design §5.2/§3.2（.easyefi 标记）；Contract/FsOps.h、VolumeModel.h。
**依赖**：S05 契约。
**接口规格**（FsOps.h 细化）：
```c
typedef struct FS_OPS FS_OPS;   // vtable: Open/Read/Write/Close/Flush/Delete/Rename/MkDir/Enum/Info
typedef struct FS_HANDLE FS_HANDLE;
EFI_STATUS FsOpen (FS_OPS*, CONST CHAR16 *Path, UINT32 Mode, FS_HANDLE**);  // READ/WRITE/CREATE
EFI_STATUS FsRead (FS_OPS*, FS_HANDLE*, VOID *Buf, UINTN *Len);
EFI_STATUS FsWrite (FS_OPS*, FS_HANDLE*, CONST VOID *Buf, UINTN *Len);
EFI_STATUS FsClose (FS_OPS*, FS_HANDLE*);   EFI_STATUS FsFlush (FS_OPS*, FS_HANDLE*);
EFI_STATUS FsDelete (FS_OPS*, CONST CHAR16 *Path);       // 文件或空目录
EFI_STATUS FsRename (FS_OPS*, CONST CHAR16 *Old, CONST CHAR16 *New);
EFI_STATUS FsMkDir (FS_OPS*, CONST CHAR16 *Path);
EFI_STATUS FsEnum (FS_OPS*, CONST CHAR16 *Dir, FS_ENTRY **Entries, UINTN *Count); // {name,size,mtime,isDir}
EFI_STATUS FsInfo (FS_OPS*, CONST CHAR16 *Path, FS_ENTRY *Out);
// 卷层：
EFI_STATUS VolEnumerate (VOLUME_INFO **Vols, UINTN *Count);  // {fs,label,capacity,free,isP3,dp}
EFI_STATUS VolFindP3 (CONST UINT8 *BootDp, UINTN BootLen, FS_OPS **P3Fs); // §5.2 五步含标记兜底
FS_OPS *FsPosixBackend (CONST CHAR8 *RootDir);   // 宿主测试用
```
规则：UEFI 后端走 SimpleFileSystem（NTFS 依赖已加载驱动）；Write 后 Flush 语义明确；Enum 目录优先按名排序；P3 发现失败返回 EFI_NOT_FOUND（Launcher 据此降级）。
**宿主测试**（POSIX 临时目录）：T09-1..8 open/read/write/close/flush/delete/rename/mkdir/enum 全操作+边界（不存在/覆盖/空目录/非空目录删除拒绝）；T09-9 大文件分块读（1MB）；T09-10 P3 发现算法（合成设备路径+mock 卷集：正常/多 NTFS 盘/克隆盘/无标记/无 P3）；T09-11 卷枚举排序与 isP3 标记；T09-12 ASan。
**QEMU 冒烟**：ntfs 变体：VolEnumerate 打印 P3 标记 → VolFindP3 成功 → P3 上写文件→Flush→Close→重开读回一致 → serial PASS。
**DoD**：T09 全绿 + 冒烟 + 交接。**禁止**：驱动加载（S14/S15 职责）；磁盘/分区级写（MVP 冻结）。

### S10 — ExecImageLib（UEFI 胶水，规模 M）

**使命**：ExecOps 真实后端——exec .efi / 嵌套 Shell 执行 .nsh / BootNext / auto: 候选（排除启动盘）。
**独占目录**：`EasyEfiPkg/Library/ExecImageLib/`
**输入**：design §5.6（动作语义）、D9/D10、评审 F01/F03/F20；Contract/ExecOps.h、ExecPath.h、VolumeModel.h、FsOps.h。
**依赖**：S04/S05/S09 契约。
**接口规格**（ExecOps.h 细化）：
```c
EFI_STATUS ExecEfi (FS_OPS *Fs, CONST CHAR16 *FullPath, CONST CHAR16 *LoadOptions,
                    EFI_STATUS *ImageReturn);   // LoadImage+StartImage；事件清理/控制台恢复
EFI_STATUS ExecNsh (CONST CHAR16 *ShellFullPath, CONST CHAR16 *ScriptFullPath,
                    CONST CHAR16 *Args);        // LoadOptions=L"<script> [args]"（隐式 -nostartup）
EFI_STATUS ExecBootNext (UINT16 Number);        // BootNext(BS|RT) + ResetSystem cold
EFI_STATUS ExecReset (VOID);
EFI_STATUS ExecAutoCandidates (CONST CHAR16 *RelPath, UINT8 *BootDp, UINTN BootLen,
                               EXEC_CANDIDATE **Out, UINTN *Count);  // 排除启动盘
```
规则：ExecEfi 前关闭自身定时器事件、保存/恢复 ConOut 模式；StartImage 返回后恢复并报告 ImageReturn；ExecNsh 的 LoadOptions 构造必须精确（脚本全限定路径+空格+参数，UCS-2）；auto: 候选用 VolSameDisk 排除启动盘全部分区。
**宿主测试**（mock 记录器）：T10-1 LoadOptions 构造黄金对比（含空格路径/带参数/空参数）；T10-2 BootNext 变量值编码（UINT16 LE）；T10-3 auto: 排除逻辑（合成卷集）；T10-4 事件清理/控制台恢复调用序列断言。
**QEMU 冒烟**：fat 变体（挂 2 个 USB 镜像）：exec 测试 .efi（打印标记+返回非零）→ 断言返回状态捕获；exec 测试 .nsh（echo 标记）→ 断言标记出现**恰好 1 次**（无 startup.nsh 递归）；auto: 候选列表不含本盘（serial 打印候选数）。
**DoD**：T10 全绿 + 冒烟 + 交接。**禁止**：在库里做菜单/确认 UI（调用方职责）；-s 之类不存在的 Shell 选项（F01）。

### S11 — Launcher（core+glue，规模 L）

**使命**：启动菜单应用——JSON 工作项 + 内置系统项 + 降级模式 + 倒计时 + 状态栏。
**独占目录**：`EasyEfiPkg/Application/Launcher/`
**输入**：design §5 全节、§6、§3.3/3.4（EASYEFI_RAN）；契约：TuiOps/ExecOps/FsOps/ConfigSchema/ExecPath。
**依赖**：S03/S04/S06/S07/S08/S09/S10 契约（stub/mock 先行开发，集成换真实）。
**交付物**：core（菜单状态机）+ glue（入口/后端装配/P3 发现/变量读取）+ .inf/碎片 + 宿主测试 + 冒烟 + 交接。
**core 规格**：状态 = {倒计时, 主菜单, 确认, 错误页, boot 子菜单, 帮助}；输入 = 配置模型 + 卷状态（P3 OK/DEGRADED）+ 按键；输出 = TuiOps 绘制 + ExecOps 调用。内置系统项固定：FileManager/Editor/boot:menu/Shell/Reset。arch 不匹配灰显；danger 强制确认默认 N；R=重扫+重载 JSON；skip_timeout ESC→Shell。
**glue 规格**：入口 SetWatchdogTimer(0)；自身 LoadedImage 设备路径 → VolFindP3；读 launcher.json（FsOps）→ JsonDomFromEdk2 → ConfigValidate；失败→内置菜单+错误横幅；读 SecureBoot/SetupMode 变量显示；任何正常退出先 SetEnv EASYEFI_RAN=yes（Shell 协议可用时）。
**宿主测试**（mock ops + 脚本按键）：T11-1 正常配置菜单渲染黄金快照；T11-2 选择+Enter→ExecOps 调用参数断言；T11-3 danger→确认 N 取消/Y 执行；T11-4 P3 缺失→DEGRADED 横幅+data: 灰显+系统项可用；T11-5 坏 JSON→内置菜单+错误横幅；T11-6 倒计时 ESC→退出调用；T11-7 arch 灰显不可选；T11-8 R 重载配置；T11-9 boot:menu 子菜单（mock BootOrder）；T11-10 .nsh 项→ExecNsh 参数断言。
**QEMU 冒烟**：fat+ntfs 变体各一：启动→菜单标记；选测试项→exec 标记；ESC→Shell 标记；ntfs 变体断言 P3:OK，改名驱动后断言 DEGRADED（V-06 前置）。
**DoD**：T11 全绿 + 双变体冒烟 + 交接。**禁止**：core 里调 gBS/gST；实现 FM/Editor 功能（只 exec 它们）。

### S12 — FileManager（core+glue，规模 L）

**使命**：刷机导向文件管理器 MVP（design §7.2 范围冻结）。
**独占目录**：`EasyEfiPkg/Application/FileManager/`
**输入**：design §7.2/§7.4、§5.6；契约：TuiOps/FsOps/ExecOps。
**依赖**：S06–S10 契约。
**core 规格**：状态 = {卷列表, 目录视图, 预览, 过滤, 搜索结果, 各类确认, 新建输入}；目录导航（进入/上级/ESC 逐级返回）；排序目录优先按名；键入即过滤；S 递归文件名搜索（FsEnum DFS，结果列表跳转）；预览 bounded ≤PcdPreviewMaxBytes（文本头/二进制 size+hex 头）；执行确认框=**全路径+文件大小**；F4 文本→Editor exec、二进制拒绝；F5 复制（跨卷，覆盖确认）/F6 改名/F7 新建目录或文件/F8 删除（文件或空目录，确认）；R 刷新；状态栏常驻全路径。
**glue 规格**：入口装配后端；卷列表来自 VolEnumerate；exec 走 ExecOps；Editor 路径按架构解析 \EFI\Apps\Editor.efi。
**宿主测试**（POSIX 临时树 + mock ops）：T12-1 导航/排序/上级；T12-2 过滤；T12-3 递归搜索命中与跳转；T12-4 预览 bounded（1MB 文件只读 16KiB，断言读取量）；T12-5 二进制 hex 头渲染；T12-6 执行确认含全路径+大小；T12-7 复制/覆盖确认/改名/删除/空目录删除/非空拒绝/mkdir/新建；T12-8 F4 文本→ExecEfi(Editor) 参数、二进制拒绝；T12-9 卷切换；T12-10 80×25 布局快照；T12-11 刷新后外部新增文件可见。
**QEMU 冒烟**：ntfs 变体：进 P3→列目录→预览脚本→复制文件→执行测试 .nsh（标记）→serial 断言链。
**DoD**：T12 全绿 + 冒烟 + 交接。**禁止**：移动/递归目录操作/内容搜索/双栏批量/磁盘级操作/鼠标（§7.2 不做清单）。

### S13 — Editor（core+glue，规模 L）

**使命**：脚本导向编辑器 MVP（design §7.3 范围冻结），UTF-8 字节往返保留。
**独占目录**：`EasyEfiPkg/Application/Editor/`
**输入**：design §7.3/§7.4；契约：TuiOps/EditorModel/FsOps、Utf8.h。
**依赖**：S01/S02/S06–S09 契约。
**core 规格**：命令分发（打开/保存/另存为/导航/插入删除/回车/查找上下/跳行/撤销/退出三键确认）；渲染=ModelRenderRow 投影+行号栏+状态栏（文件名+*+行:列+编码+BOM 标记）；非 ASCII 占位黄色，光标处状态栏显示 U+XXXX；打开经 FsOps 读字节→ModelLoad（超限拒绝并提示）；保存 ModelSave→FsOps 写+Flush+Close；另存为输入行（AsciiOnly 文件名）。
**glue 规格**：入口装配；命令行参数=待打开文件全路径（FM F4 传入）；退出返回码约定（0 正常）。
**宿主测试**（POSIX 文件 + mock 屏幕）：T13-1 打开中文注释脚本→编辑 ASCII→保存→**非 ASCII 字节逐字节相等**；T13-2 BOM/CRLF 保留；T13-3 修改标记生命周期（撤销回保存点消失）；T13-4 退出三键（保存/放弃/取消）各路径；T13-5 查找上下/大小写；T13-6 跳行；T13-7 撤销；T13-8 超限拒绝；T13-9 另存为新文件；T13-10 状态栏 U+XXXX 显示；T13-11 80×25 渲染快照；T13-12 保存失败（只读目录）错误页不崩。
**QEMU 冒烟**：ntfs 变体：打开 P3 脚本→插入标记文本→保存→退出→重开读回断言（V-03/V-05 前置）。
**DoD**：T13 全绿 + 冒烟 + 交接。**禁止**：替换/多文件/hex 编辑/语法高亮/GOP/鼠标；>256KiB 编辑。

### S14 — ntfs-3g 集成（第三方构建，规模 S）

**使命**：锁定 commit 拉取 + 双架构构建 + 驱动级验证，产出 ntfs.efi（X64/AA64）。
**独占目录**：`third_party/ntfs-3g/`（拉取脚本+补丁记录）、`docs/pins/S14-ntfs3g.md`。
**输入**：design §7/§8；上游 README（load+map -r 用法、自动卸载特性、GPLv2）。
**依赖**：S00 构建配方；无代码依赖。
**交付物**：fetch 脚本（锁 commit，记录于 `docs/pins/S14-ntfs3g.md`）、双架构构建脚本（uefi-driver.dsc，GCC5_AARCH64_PREFIX）、产物清单+SHA256、驱动冒烟证据、GPLv2 源码提供指引文本。
**验证**：T14-1 双架构产物存在且 PE 机器类型正确（objdump）；QEMU ntfs 变体：load ntfs.efi→map -r→P3 可见→写文件→**重启→读回一致**（V-05 驱动部分）；最后句柄关闭后拔盘模拟（QEMU 移除设备）无挂死。
**DoD**：构建可复现（干净 worktree 重跑成功）+ 冒烟 + 交接。**禁止**：改上游源码（必要时补丁走 leader 评审）；FORCE_READONLY（要读写）。

### S15 — 引导脚本与启动链（规模 M）

**使命**：startup.nsh（严格+兼容两版）+ 启动链 QEMU 实证（design §3.3/3.4，评审 F02/F14/F25 的落地）。
**独占目录**：`boot/`
**输入**：design §3 全节；S00 TestLauncherStub；S17 Shell 产物（可先用 stub Shell 脚本占位，集成换真实）。
**依赖**：S00 测试镜像/桩；S14 驱动（ntfs 变体）。
**交付物**：boot/startup.nsh（严格，两 ESP 逐字节相同）、boot/startup-compat.nsh（try-both+EASYEFI_RAN）、根目录兜底副本规则、expect 断言脚本集、启动链测试报告。
**验证（QEMU，双架构）**：T15-1 严格布局路由（x64 命中 P1/AA64 命中 P2，V-01）；T15-2 %homefilesystem% 路径正确（驱动加载成功标记）；T15-3 ESC 倒计时跳过；T15-4 noautorun（启动分区+P3 两处）；T15-5 兼容布局 try-both 双架构（TestLauncherStub 标记架构+置 EASYEFI_RAN，断言只运行正确架构一次）；T15-6 驱动缺失→Launcher 仍启动（stub 降级标记）；T15-7 load 失败不中止脚本（F25 语义实证）。
**DoD**：T15 全绿（双架构）+ 交接。**禁止**：脚本里检测 load 状态做降级（F25：测不到，降级归 Launcher）；硬编码 fsN。

### S16 — 打包与写盘流水线（规模 M）

**使命**：mkimage/fix-gpt 脚本 + GPT 正确性实证（design §8，评审 F06/F07/F08/F18 的落地）。
**独占目录**：`scripts/mkimage.sh`、`scripts/fix-gpt.sh`、`scripts/verify-image.sh`（S00 的 build-module/mktestimage/qemu-run 归 leader，不碰）。
**输入**：design §3.1/§3.2/§8；分区契约。
**依赖**：S14/S17 产物清单（可先用占位文件测布局）。
**交付物**：mkimage.sh（IMG_SIZE_GB 参数+载荷尺寸校验、sgdisk EF00/EF00/**0700**、losetup --show、mkfs.vfat/mkfs.ntfs、文件树拷贝、.easyefi 标记写入版本+UUID）、fix-gpt.sh（sgdisk -e + 可选扩容 P3+ntfsresize）、verify-image.sh（gdisk/sgdisk 输出解析断言）、Windows 写盘指引（Rufus DD/Etcher）文档节。
**验证**：T16-1 loop 设备 GPT 断言（三类型 GUID/分区号/对齐 1MiB/备份头位置）；T16-2 尺寸约束拒绝（载荷>可用）；T16-3 .easyefi 标记内容；T16-4 模拟 dd 到更大 loop 盘→备份头错位→fix-gpt 修复→断言；T16-5 扩容后 NTFS 可挂载且容量正确；T16-6 整镜像 QEMU 双架构引导到 Shell（集成预检）。
**DoD**：T16 全绿 + 交接。**禁止**：签名步骤（SB 关闭）；8300 GUID（F07）；硬编码 loop0（F18）。

### S17 — Shell 构建固化（第三方构建，规模 S）

**使命**：锁定 edk2 tag 的原版 Shell 双架构构建 + PCD 配置化。
**独占目录**：`scripts/build-shell.sh`、`docs/pins/S17-shell.md`、`config/shell-pcds.dsc.inc`。
**输入**：design §8；评审 F14（倒计时 PCD）。
**交付物**：build-shell.sh（X64/AA64，GCC5）、PCD 覆盖碎片（PcdShellDefaultDelay 等可配置）、产物 BOOTX64.EFI/BOOTAA64.EFI + SHA256 清单、版本字符串记录。
**验证**：T17-1 双架构产物 PE 机器类型；QEMU：引导到 Shell 提示符（serial 标记）、ver 输出记录、倒计时秒数与 PCD 一致、ESC 跳过生效。
**DoD**：可复现构建 + 验证 + 交接。**禁止**：魔改 Shell 源码（需求 R4 零回归）；集成 ufm 等第三方命令（Q4 全退）。

---

## 4. 契约变更流程（唯一同步点）

1. 提案流在交接注记写 `契约变更提案`：动机/签名 diff/影响流清单。
2. leader 裁决：批准 → 更新 Include/Contract/ + 版本号 + 通知所有消费流（任务列表消息）；拒绝 → 提案流按原契约实现。
3. 消费流收到变更通知后：更新 stub 依赖、重跑宿主测试、在交接注记记录适配。
4. **数据契约**（schema/标记格式/分区布局）变更同理，且必须同步 design.md（leader 独占）。

## 5. Wave 2 — 集成（leader + 2–3 agent）

### S18 — 全量集成与发布候选
- 按依赖序合并各流分支（纯逻辑→胶水→应用→boot/打包），每合并一流跑跨模块回归；冲突以契约为准，应用代码让路。
- 整镜像（S16）+ V-01–V-12 全矩阵（design §9.1），重点 **V-08 刷机闭环**（FM 找固件→F4 改脚本→保存→执行→嵌套 Shell→exit 回 Launcher）与 V-03 UTF-8 往返、V-05 持久化。
- 兼容布局 vs 严格布局：汇总 S15 双架构证据，实机矩阵后决定默认（D3）。
- 发布物：镜像 + SHA256SUMS + 用户文档（SB 关闭步骤/BitLocker 警告/厂商工具 drop-in 指引/源码提供指引）+ 实机验证记录模板。
- 出口：V 矩阵全绿 + 文档齐 + 实机计划就绪（Ampere + ≥2 x64 固件厂）。

## 6. 编排备注（给 leader）

- 派发顺序：S00 验收 → Wave 1 全量并行（≤17）→ 各流 DoD 验收（看测试证据，抽查契约边界与文件写/exec 危险操作代码）→ S18。
- 资源预算：并发 QEMU ≤4（2GB/实例，信号量）；宿主测试不限；worktree 磁盘 ~2GB/流。
- 关键路径：S00 → S09/S10（胶水）→ S11/S12/S13（应用）→ S18；纯逻辑流（S01–S05）与构建流（S14/S16/S17）不在关键路径上，可最先完成并反哺 stub 替换。
- 规模分布：S/M/L 约 6/8/5，L 流（S00/S09/S11/S12/S13）建议派给能力最强的 agent 或拆子任务（core 与 glue 可再拆两人，契约不变）。
