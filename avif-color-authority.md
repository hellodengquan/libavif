# AVIF 里 ICC 与 CICP 各自管什么

> 面向实现预览的同事：从素材解出 YUV 平面、转到 RGB、再交给支持色彩管理的显示端，这条链路上每一步依据哪份信息。
> 分析对象：本仓库的 libavif 1.4.2（`CMakeLists.txt:34`、`include/avif/avif.h:80-82`）。
> 文中所有数值都由本仓库构建出的 `build/libavif.16.4.2.dylib` 实测得到（构建配置：仅 AOM，libyuv / libsharpyuv 关闭，因此走的是 `src/reformat.c` 里的内置浮点路径）。复现方法见文末。

---

## 0. 结论先行

1. **ICC 和 CICP 在同一个文件里并存，是 AVIF/HEIF 的常态，不是二选一。** 编码器写 ICC 时仍然会写一个 `nclx`（`src/write.c:739-741` 明说 "*always* writing an nclx box"）。
2. **libavif 内部只有一条色彩链路，它只吃 CICP，完全不读 ICC。** `src/reformat.c`、`src/reformat_libyuv.c`、`src/reformat_libsharpyuv.c`、`src/colr.c`、`src/colrconvert.c` 里对 `image->icc` 的引用数为 **0**（库源码 `src/` 里只有 `src/read.c`、`src/write.c` 搬运字节，以及 `src/gainmap.c:339`、`861` 用来**拒绝** ICC）。库内没有 lcms/ICC 解析引擎。
3. **在 YUV→RGB 这条（解码）链路里，`transferCharacteristics` 一次都没被用到。** 实测把 TC 改成 13/1/16/18/8，YUV→RGB 结果逐字节相同。全库读 TC 做实际运算的只有两处，都与这条链路无关：`src/reformat_libsharpyuv.c:28-29`（RGB→YUV 编码侧的下采样优化，本机构建未启用）和 `src/gainmap.c:607-608`（gain map 音调映射）。所以"转成 RGB"这一步产出的不是线性光，而是**仍带某种伽马编码、且仍在编码端原色域里**的 RGB。
4. **真正决定 YUV→RGB 数值的只有两个字段：`matrixCoefficients` 和 `yuvRange`。** `colorPrimaries` 只在 `matrixCoefficients == 12`（色度派生）时被间接用到（`src/colr.c:137-141`）。
5. **声明冲突时，libavif 的裁决规则只有一条：`colr` 的 `nclx` 整体压过 AV1 码流里的 CICP（四个字段一起换），ICC 不参与任何裁决。** ICC 与 CICP 的矛盾被原样导出给调用方（`include/avif/avif.h:796-803` 就是在提醒调用方这一点）。
6. **推论：ICC 无法替代 CICP 的 MC。** ICC 描述的是 RGB→PCS 的变换，天然不含 YUV 矩阵和分量范围。所以"有 ICC 就完全忽略 CICP"是错的——步骤 1（YUV→RGB）必须用 CICP 的 MC/range；ICC 只接管步骤 2、3（反伽马、色域换基）。

---

## 1. 两套元数据在文件里的位置

| 信息 | 容器位置 | 库内字段 | 源码 |
|---|---|---|---|
| ICC profile | `colr` box，`colour_type` = `rICC` 或 `prof` | `avifImage.icc`（不透明字节） | 解析 `src/read.c:2592-2611`；写出 `src/write.c:725-738` |
| CICP + 范围 | `colr` box，`colour_type` = `nclx` | `colorPrimaries` / `transferCharacteristics` / `matrixCoefficients` / `yuvRange` | 解析 `src/read.c:2611-2625`；写出 `src/write.c:662-681` |
| 同一组 CICP + `color_range` | AV1 Sequence Header OBU 的 `color_config()` | 解码后由 codec 层回填到同一组字段 | `src/obu.c:309-318`；`src/codec_aom.c:241-243`；`src/codec_dav1d.c:192-197` |
| （实验性）迷你头 `mini` | `mif3`/`mini` box，字段名 `colour_primaries` 等 | 解析后**再翻译成**一个 `nclx` 属性挂上去（本机构建未启用，仅读码） | `src/read.c:4192-4290`、`4606-4627` |

要点：

- **`nclx` 是四个字段绑在一起的整体**：CP、TC、MC、`full_range_flag`。没有任何机制允许"CP 取 A 源、MC 取 B 源"。
- **ICC 和 `nclx` 可以同时存在**，规范（HEIF 6.5.5.1 Amd.3）只限制"每种 `colour_type` 最多一个"（`src/read.c:5786-5805` 会因重复而报 `BMFF_PARSE_FAILED`）。
- 写侧印证：`avifEncoderWriteColorProperties()` 先按需写 `prof`，然后**无条件**再写一个 `nclx`（`src/write.c:708-742`）。

---

## 2. 解码时谁覆盖谁：源码与实测

### 2.1 `nclx` 压过 AV1 码流

机制是一个 `cicpSet` 布尔量：

- `src/read.c:1071-1076` 声明了它的用途，并引 MIAF 7.3.6.4：
  > "The colour information property takes precedence over any colour information in the image bitstream, i.e. if the property is present, colour information in the bitstream shall be ignored."
- `src/read.c:6715-6723`：先把 `colr` 属性读进 `decoder->image`，读的过程中一旦发现 `nclx` 就把 `data->cicpSet` 置真（`src/read.c:5761-5763`）。
- `src/read.c:6757-6786`：**只有** `!data->cicpSet` 时才去解析第一个 sample 的 sequence header 来"捞" CICP。

实测（同一份 AVIF，只改文件里的 `nclx` 字节，不改码流）：

| 文件状态 | `decoder->image` 的 CICP | `avifImageYUVToRGB` 输出 |
|---|---|---|
| `nclx` 1/13/6，OBU 1/13/6 | 1/13/6 | R=181 G=47 B=142 |
| `nclx` 改成 1/13/**1**，OBU 仍 1/13/**6** | 1/13/**1** | R=**191** G=**65** B=**144** |
| 把 `nclx` 的 `full_range_flag` 改成 1，OBU 仍 limited | range=**full** | R=**173** G=**55** B=**139** |
| 把 `nclx` 的 `colour_type` 改名成 `free`，只留 OBU（该文件 OBU 是 9/16/9） | 9/16/9 | R=185 G=60 B=145 |

两个细节值得注意：

- 覆盖是**整体**的。第三个实验里范围被换成 full 之后，YUV 样本值本身没变（解码出来仍是 Y=100,U=150,V=180），变的只是"怎么解释这些数"。所以**一个声明字段的冲突会直接改变像素数值**，不只是改变元数据。
- 范围不一致时 libavif **不报错**。`src/read.c:6918-6941` 明确写了：AV1-ISOBMFF 要求 `colr.full_range_flag` 与 OBU 的 `color_range` 一致，但"历史上有编码器没写，所以放行"，并保留 `colr` 的值。这是**有意容忍的静默错误**。

### 2.2 ICC 不参与任何裁决

`avifReadColorProperties()`（`src/read.c:5780-5806`）的顺序是：

1. 扫一遍属性，看到 `hasICC` 就把字节读进 `avifRWData * icc`（仅此而已，不解析内容）；
2. 再调 `avifReadColorNclxProperty()` 填 CICP。

两者之间**没有任何交叉校验**。实测：给一份 `nclx` 1/13/6 的文件塞进一个 128 字节的 ICC 载荷，解码后 `icc.size=128`、CICP 仍是 1/13/6、`avifImageYUVToRGB` 输出仍是 181/47/142 —— **加 ICC 对 libavif 的像素数值零影响**。

库唯一"承认 ICC 优先"的地方是 `apps/shared/avifpng.c:881` 这句注释：

> `// With AVIF, an ICC profile takes priority over CICP, but with PNG files, CICP takes priority over ICC.`

这是**应用层的约定**（写 PNG 时有 ICC 就不写 CICP，见 `avifpng.c:783-790`、`881-891`），不是库内实现的规则。

### 2.3 编码侧的行为，影响你会拿到什么样的文件

- `avifenc` 的默认 CICP 是 **CP=2、TC=2、MC=6**（`apps/avifenc.c:1550-1552`），默认范围是 **full**（`apps/avifenc.c:1534`）。
- `avifenc`：源图既没 ICC 也没 CICP 时，**显式改写成 sRGB**（CP=1, TC=13），理由是"2/2/x 反正会被当作 sRGB"（`apps/avifenc.c:2510-2517`）。注意这是**只在没有 ICC 时**才发生——有 ICC 时 CP/TC 保持 2/2。
- 从 PNG 的 cHRM/gAMA 推导出 ICC 时，把 CICP 主动置为 Unspecified（`apps/shared/avifpng.c:482-494`）——即"色彩语义交给 ICC，CICP 只留 MC 给 YUV 用"。
- 库编码器写 OBU / mini 的默认 CICP 取决于**有没有 ICC**：`hasIcc ? 2 : BT709`、`hasIcc ? 2 : SRGB`（`src/write.c:2771-2774`；mini 同款逻辑在 `src/read.c:4278-4284`）。

**这就是最常见的"ICC 文件"形态：`colr` 里有 `prof`，`nclx` 里 CP=2 TC=2，MC=6** —— 一份文件里语义信息只在 ICC 里，矩阵信息只在 CICP 里。第 5 节末尾会说明这类文件为什么最容易踩坑。

---

## 3. 各阶段依据哪份信息

把链路拆成四步，每步只依赖明确的输入：

```
        ┌─ 步骤 1：YUV 平面 → RGB 整数  ← libavif 完成
文件 ───┤      依据：matrixCoefficients（+ colorPrimaries，仅 MC=12）、yuvRange
        │
        ├─ 步骤 2：RGB 整数 → 线性光     ← 显示端/CMS 完成
        │      依据：transferCharacteristics 或 ICC 的 TRC 曲线
        │
        ├─ 步骤 3：编码原色域 → 显示原色域  ← 显示端/CMS 完成
        │      依据：colorPrimaries 或 ICC 的 colorant/白点标签
        │
        └─ 步骤 4：线性 → 显示编码值      ← 显示端完成
               依据：显示设备 profile
```

| 阶段 | 依据字段 | 明确不依据 | 源码 |
|---|---|---|---|
| 解封装 / 取出 YUV 平面 | 无色彩依赖（`yuvFormat`、`depth`、`yuvRange` 供后续用） | ICC | `src/read.c:6715-6790` |
| 限幅 → 全幅归一化 | **`yuvRange`** | ICC、TC、CP | `src/reformat.c:153-156`、`583-605` |
| 色度上采样（4:2:0/4:2:2） | `yuvFormat`（**`yuvChromaSamplePosition` 被忽略**） | — | `src/reformat.c:700-845` |
| YUV→RGB 矩阵 | **`matrixCoefficients`**；仅 MC=12 时用 `colorPrimaries` | **`transferCharacteristics`**、ICC | `src/colr.c:120-187`、`src/reformat.c:877-880` |
| 输出 RGB 的量化 | 目标 RGB 的 depth（默认同源 depth） | — | `src/reformat.c:953-965` |
| 反伽马（linearize） | **库内不做**；显示端用 TC 或 ICC | — | `src/colr.c:473-491` 有函数（gain map 用）但转换路径不调用 |
| 原色域换基 | **库内不做**（除 gain map）；显示端用 CP 或 ICC | — | `src/colrconvert.c:94-176` 有矩阵函数但转换路径不调用 |

关于 alpha：有限范围的 α 平面在解码时会被**转成全幅**（`src/read.c:6842-6881`，调用点 `6936-6945`）。注意这不是无条件转换——只有 codec 层报告 `isLimitedRangeAlpha` 时才转，因为限量 alpha 已从规范移除但旧文件仍可能存在。转完之后，交给显示端的 alpha 一律是 full range。

关于 gain map：这是库内**唯一**会做"反伽马 + 3×3 原色域矩阵 + 重新编码"的地方（`src/gainmap.c:146-170`、`183-190`、`265-288`），但它**显式拒绝 ICC**：

```c
// src/gainmap.c:339-341
if (baseImage->icc.size > 0 || gainMap->altICC.size > 0) {
    avifDiagnosticsPrintf(diag, "Tone mapping for images with ICC profiles is not supported");
    return AVIF_RESULT_NOT_IMPLEMENTED;
}
```

即：带 ICC 的 HDR AVIF 的音调映射，libavif 明确交给显示端。

---

## 4. 一组明确的算例

### 4.1 输入

| 项 | 值 |
|---|---|
| 平面采样 | `Y = 100`，`Cb = 150`，`Cr = 180` |
| 位深 | 8 bit |
| 像素格式 | 4:4:4（避开上采样的干扰） |
| `yuvRange` | limited |
| `matrixCoefficients` | 6（BT.601） |
| `colorPrimaries` | 1（BT.709/sRGB，用于步骤 3，**不参与步骤 1**） |
| `transferCharacteristics` | 13（sRGB，用于步骤 2，**不参与步骤 1**） |

系数取值来自 `src/colr.c:120-135`：`kr = 0.299`，`kb = 0.114`，`kg = 1 − kr − kb = 0.587`。

### 4.2 库已经做完的部分（步骤 1）

**归一化**（`src/reformat.c:153-156`，8 bit limited：`biasY=16, rangeY=219, biasUV=128, rangeUV=224`）：

```
Y'  = (100 − 16) / 219 = 34/219  = 0.383561644
Cb' = (150 − 128) / 224 = 22/224 = 0.098214286
Cr' = (180 − 128) / 224 = 52/224 = 0.232142857
```

查表实现见 `src/reformat.c:583-605`（`(cp − bias) / range`）。

**矩阵**（`src/reformat.c:877-880`，即 H.273 公式 20-31）：

```
R' = Y' + 2(1 − kr)·Cr' = 0.383561644 + 2×0.701×0.232142857 = 0.709025930
B' = Y' + 2(1 − kb)·Cb' = 0.383561644 + 2×0.886×0.098214286 = 0.557597358
G' = Y' − [2(kr(1−kr)Cr' + kb(1−kb)Cb')] / kg
   = 0.383561644 − [2(0.299×0.701×0.232142857 + 0.114×0.886×0.098214286)] / 0.587
   = 0.183980906
```

**量化**（`src/reformat.c:889-892` 先 `AVIF_CLAMP`，`src/reformat.c:953-965` 再 `0.5 + x×255` 截断）：

```
R = int(0.5 + 0.709025930×255) = 181
G = int(0.5 + 0.183980906×255) =  47
B = int(0.5 + 0.557597358×255) = 142
```

**实测一致**：`avifImageYUVToRGB` 返回 `R=181 G=47 B=142`。

这 142 号蓝、47 号绿，就是 libavif 交给你的"RGB"。注意它**不是线性光**，也**没有换到显示器的原色域**——它仍然是"编码端声明的原色域（这里 CP=1）＋编码端声明的传递函数（这里 TC=13）＋源位深"下的整数编码值。libavif 只做了"YUV 分量 → 编码域 RGB 分量"这一步。

### 4.3 显示端还需要做的部分（步骤 2–4）

设显示端是标准 sRGB。

**步骤 2：反伽马。** 用 TC=13 的 sRGB EOTF（`src/colr.c:369-392` 有常数一致的分段公式，可作为对拍参考）：

```
R_lin = ((181/255 + 0.055) / 1.055)^2.4 = 0.462081
G_lin = (( 47/255 + 0.055) / 1.055)^2.4 = 0.028428
B_lin = ((142/255 + 0.055) / 1.055)^2.4 = 0.270503
```

**步骤 3：原色域换基。** CP=1 = sRGB = 显示原色域，矩阵退化为单位阵（用库内 `avifColorPrimariesComputeRGBToRGBMatrix(1, 1, m)` 实测确实输出单位阵，`src/colrconvert.c:157-176`）。所以线性值不变。

**步骤 4：显示编码。** 仍是 sRGB OETF，回到 (181, 47, 142)。端到端无损，符合预期。

若显示端是 **Display P3**（CP=12），步骤 3 的矩阵非平凡（Bradford 适应到 D50 再转，`src/colrconvert.c:94-155`）：

```
sRGB 线性 (0.462081, 0.028428, 0.270503)
  --[sRGB→P3 矩阵]-->  (0.385091, 0.042823, 0.256250)
  --[sRGB OETF]-->     P3 编码值 167, 58, 139
```

也就是同一批像素，在 P3 屏上应该发 (167, 58, 139)，而不是 (181, 47, 142)——**这 (−14, +11, −3) 的码值差别，就是显示端那"尚需承担的工作"**。libavif 交给你 (181, 47, 142) 时，它一个数都没替你算。

### 4.4 同组样本在其它声明下的结果（全部实测）

固定 `Y=100 Cb=150 Cr=180`（10/12 bit 时按 `bias/range` 等比放大为 400/600/720 与 1600/2400/2880），4:4:4：

| `yuvRange` | MC | depth | 输出 RGB（源位深码值） | 归一化上等价于 |
|---|---|---|---|---|
| limited | 6 BT.601 | 8 | 181, 47, 142 | 基准 |
| limited | 1 BT.709 | 8 | **191, 65, 144** | 同一批样本，换矩阵 |
| full | 6 | 8 | **173, 55, 139** | 同一批样本，换范围解释 |
| limited | 2 Unspecified | 8 | 181, 47, 142 | 回退到 BT.601（`src/colr.c:174-176`） |
| limited | 15 保留值 | 8 | 181, 47, 142 | **同样静默回退到 BT.601**（查表未命中） |
| full | 0 Identity (444) | 8 | **180, 100, 150** | 平面被当成 G,B,R 直接重排（`src/reformat.c:851-856`） |
| limited | 0 Identity (444) | 8 | **191, 98, 156** | 平面被当成 G,B,R 直接重排（`src/reformat.c:851-856`），且 U/V 复用亮度表（`src/reformat.c:590-593`），所以 G/B 也吃了 Y 的偏置 |
| limited | 12 ChromaDerived, CP=1 | 8 | 191, 65, 144 | kr=0.21264 |
| limited | 12 ChromaDerived, CP=12 P3 | 8 | 189, 62, 144 | kr=0.22897 |
| limited | 12 ChromaDerived, CP=9 BT.2020 | 8 | 185, 60, 145 | kr=0.26270 |
| limited | 14 ICtCp | 8 | **`AVIF_RESULT_REFORMAT_FAILED`** | 不支持（`src/reformat.c:129-136`） |
| limited | 6 | 10 | 725, 188, 570 | 归一化后与 8 bit 相同（仅量化误差） |
| limited | 6 | 12 | 2903, 753, 2283 | 同上 |

两点工程提醒：

- **MC=2 和 MC=15 不会报错**，都会静默按 BT.601 处理。这对"ICC 文件"很关键：`nclx` 里 MC 常是 6，恰好也是 601，看不出问题；但如果一个 MC=2 的文件实际是用 709 矩阵编的，你无从得知。MC=15 是保留值，落到 601 纯属查表未命中的副作用。
- **其余 MC 会直接失败**（`avifImageYUVToRGB` 返回 `AVIF_RESULT_REFORMAT_FAILED`）。8 bit 4:4:4 实测：

  | 范围 | 可用 | 失败 |
  |---|---|---|
  | limited | 0, 1, 2, 4, 5, 6, 7, 9, 12, 15 | 3, 8, 10, 11, 13, 14, 16, 17, 18 |
  | full | 0, 1, 2, 4, 5, 6, 7, 8, 9, 12, 15 | 3, 10, 11, 13, 14, 16, 17, 18 |

  其中 8（YCgCo）只在 full range 可用，16/17（YCgCo-Re/Ro）还额外要求源位深比目标 RGB 位深大 1 或 2（`src/reformat.c:167-174`），所以 8 bit 源永远用不了。预览必须处理这个返回值，不能假定成功。

### 4.5 同一个 RGB 三元组在显示端的三种反伽马（实测）

如果显示端把 TC 搞错，(181, 47, 142) 会被解释成：

| 显示端误用的 EOTF | 线性值 | 重新编码为 sRGB 后的码值 | 偏差 |
|---|---|---|---|
| sRGB（正确） | 0.46208, 0.02843, 0.27050 | 181, 47, 142 | — |
| gamma 2.2 | 0.47044, 0.02422, 0.27583 | 182, 43, 143 | +1, −4, +1 |
| BT.709 曲线 | 0.50605, 0.04926, 0.31768 | 189, 63, 153 | +8, +16, +11 |

如果显示端把原色域搞错（把 sRGB 数据当 P3 解释，再转到 sRGB 屏）：

```
(181, 47, 142) --[当作 P3 反伽马]--> --[P3→sRGB]--> (197, 26, 146)  偏差 +16, −21, +4
```

可见**原色域错配的偏差量级和传递函数错配相当，方向也不同**（原色域错配绿色 −21，传递函数错配绿色 +16）。两者都会造成肉眼可见的偏色，但补偿手段完全不同：前者要在线性光域做 3×3 换基，后者只改一条一维曲线。这也解释了为什么 ICC 缺失时显示端宁可退回 sRGB 也不该猜 P3——猜错的代价更大且更难纠正。

---

## 5. 文件声明与码流不一致时，推导在哪一步改变

把完整推导写成一个式子：

```
显示码值 = OETF_disp( M_{disp←src} ( EOTF_src( RGB_code / 255 ) ) )
其中 RGB_code = quantize( YUV2RGB_{MC, yuvRange}(Y, Cb, Cr) )
```

三类冲突，改变的**步骤不同**：

### 类型 A：`colr` 的 `nclx` 与 AV1 码流里的 CICP 不一致

**改变发生在最内层：`MC` 与 `yuvRange` 的取值。** 位置在 libavif 解析阶段，像素运算之前。

- 裁决：`nclx` 胜，四个字段整体替换（`src/read.c:6715-6723`、`1071-1076`、`6757-6786`）。
- 推导式子形状不变，只是 `YUV2RGB` 的下标变了。
- **后果是像素数值真的变了**，不是"元数据标错"而已：`nclx` 说 MC=1 而码流说 MC=6 → 输出从 (181,47,142) 变成 (191,65,144)。范围冲突更明显：(181,47,142) → (173,55,139)。
- 特别注意：libavif **接受** `full_range_flag` 与 OBU `color_range` 不一致的文件（`src/read.c:6918-6941`），且以 `colr` 为准做范围换算。两种错法实测：
  - **limited 编码、却声明成 full**：偏置 16 不再扣、分母从 219 改成 255。原本的黑 (16,128,128) 输出 (16,16,16)，白 (235) 输出 (235)——**黑位抬高、整体发灰发淡**。
  - **full 编码、却声明成 limited**：按 (v−16)/219 解释。(0) 算出负值被 `AVIF_CLAMP` 压到 0，(255) 算出 1.09 被压到 1.0——**两端同时塌缩，暗部和高光细节一起丢**（`src/reformat.c:889-892` 的裁剪）。
- 对预览实现：这一步你没有选择权，库已经替你定了。但**如果你自己解析 OBU 去交叉校验，就会得到与库不同的结果**——不要那么做，按库的输出来。

### 类型 B：ICC 与 CICP 描述的色度学互相矛盾

**`YUV2RGB` 那一步完全不变**，因为它只吃 MC/range，而 ICC 里没有这两项。改变发生在**库返回之后**，即步骤 2、3 的输入源。

- 若 CICP 的 CP/TC 是 2/2（`avifenc` + ICC 的典型产物，见 `src/write.c:2771-2774`、`src/read.c:4278-4284`），那么**用 CICP 做步骤 2/3 没有意义**，必须用 ICC。
- 若 CICP 有具体值但和 ICC 冲突，libavif 不会告诉你。它只是把两者都塞进 `avifImage`。`include/avif/avif.h:796-803` 的注释就是这个意思：
  > "If your system supports ICC profiles, be sure to check for the existence of one (avifImage.icc) before relying on the values listed here!"

**正确的拆分（也是 `avifpng.c` 的做法）**：

| 步骤 | ICC 存在时 | 无 ICC 时 |
|---|---|---|
| 1. YUV→RGB | **CICP 的 MC + `yuvRange`**（ICC 无能为力） | CICP 的 MC + `yuvRange` |
| 2. 反伽马 | **ICC 的 TRC**（tag `rTRC/gTRC/bTRC`） | CICP 的 TC |
| 3. 换基 | **ICC 的 colorant + 白点**（`rXYZ/gXYZ/bXYZ`，PCS=D50） | CICP 的 CP |
| 4. 显示编码 | 显示 profile | 显示 profile |

这张表就是"ICC 优先"这句话的精确含义——**它只在步骤 2、3 上优先**，步骤 1 仍然由 CICP 说了算。

实测佐证：往同一文件加 ICC，libavif 输出一字不差（181,47,142 不变）；把 MC 从 6 改成 1，加不加 ICC 都变成 (191,65,144)。所以 ICC **不能**用来纠正矩阵错配。

### 类型 C：声明的 MC/CP 与内容真实生成方式不一致

**不改变任何步骤，只是错误被固化在步骤 1 的输出里。** libavif 没有任何检测能力（它只按表查系数）。

- 这类错误**一旦过了 `avifImageYUVToRGB` 就不可逆**——YUV 平面被丢掉后，你无法用正确的 MC 重算。
- 工程建议：**预览管线不要急着 `avifImageYUVToRGB` 之后就释放 YUV 平面**。把 `avifImage`（含 Y 平面和 `matrixCoefficients`）留着，需要重新解释矩阵时还能重跑步骤 1。
- 常见的真实场景：素材从 sRGB PNG 来，编码器按 4:2:0 默认写了 MC=6，但中间某次 RGB→YUV 是用 709 做的。这种文件在 mc=6 下解就会偏色，且没有任何字段能揭示。

### 补充：`mini`/`mif3` 头的一条隐藏规则（源码推断，本机构建未启用）

> 本仓库构建里 `AVIF_ENABLE_EXPERIMENTAL_MINI=OFF`，下面这段是读码得出，未做实测。

走实验性迷你头时，**没有显式 CICP 且带 ICC** 的文件被读成 CP=2、TC=2、**MC=6**（`src/read.c:4278-4284`），写侧对称（`src/write.c:2771-2779`）；随后这套数值被翻译成一个 `nclx` 属性挂到主图上（`src/read.c:4606-4627`），因此后面走的仍是第 2 节那条"`nclx` 优先"的路径。

也就是说，一个 `mif3` + ICC 的文件里，`MC=6` 是**容器凭空补出来的**，并非作者声明。4:2:0 时它几乎总是对的（601 是 AVIF 的实用默认），但这就构成"文件里唯一可信的色彩信息只有 ICC，而算法需要的矩阵是猜的"这一局面。预览实现在这种文件上要把 MC 当作"可覆盖的假设"，而不是"权威声明"。

---

## 6. 给预览实现的落地清单

1. **解封装后立刻记录四元组**：`(colorPrimaries, transferCharacteristics, matrixCoefficients, yuvRange)` 以及 `icc.size > 0`。这四个字段已经是 libavif 按优先级裁决后的结果，直接信它。
2. **步骤 1 只信 CICP 的 MC/range。** 有 ICC 也一样。可用性受范围和子采样约束（`src/reformat.c:124-160`、`167-174`）：limited 下可用 0（仅 4:4:4/4:0:0）、1、2、4、5、6、7、9、12、15；full 下额外可用 8；16/17 另有位深前置条件；3、10、11、13、14、18 会直接失败。必须 `try` 住并降级。
3. **别把 `avifImageYUVToRGB` 的输出当线性光。** 它是"编码端原色域 + 编码端传递函数"下的整数 RGB。`avifRGBImage.isFloat` 也只是把**同一批伽马编码值**打包成 f16（`src/reformat.c:1423-1446`、`1591-1595`），不是线性化。
4. **有 ICC 时，步骤 2/3 交给 ICC**；CICP 的 CP/TC 此时通常就是 2/2，别拿它当色彩语义。
5. **无 ICC 时，步骤 2/3 用 CICP**；若 CP/TC 仍为 2/2，按 MIAF 默认（`src/colr.c:158-172` 的注释）取 CP=1、TC=13。注意 MC 的兜底是"**任何**未列入系数表的取值都按 BT.601"，不只是 2（`src/colr.c:174-182`），所以 MC=15 这类保留值也会静默变成 601，而不是报错。
6. **保留 YUV 平面**，至少到确认色彩链正确为止，给类型 C 的冲突留一条重算路径。
7. **带 ICC 的 gain map/HDR**：`avifImageApplyGainMap` 会返回 `AVIF_RESULT_NOT_IMPLEMENTED`（`src/gainmap.c:339-341`），必须由显示端 CMS 处理，而不是指望库。
8. **范围冲突不要自己"修正"。** 若你发现 `colr` 与 OBU 的 `color_range` 不一致，libavif 已经按 `colr` 算了；自行改用 OBU 反而会和库的 RGB 输出对不上。

---

## 7. 复现方式

本仓库构建的是 1.4.2，AOM only、libyuv/sharpyuv 关闭：

```
ninja -C build avif        # 产出 build/libavif.16.4.2.dylib
```

最小探针（4:4:4、8 bit、limited，直接写平面再转 RGB）：

```c
#include "avif/avif.h"
#include <stdio.h>

static void probe(avifColorPrimaries cp, avifTransferCharacteristics tc,
                  avifMatrixCoefficients mc, avifRange range)
{
    avifImage * img = avifImageCreate(2, 2, 8, AVIF_PIXEL_FORMAT_YUV444);
    img->yuvRange = range;
    img->colorPrimaries = cp;
    img->transferCharacteristics = tc;
    img->matrixCoefficients = mc;
    if (avifImageAllocatePlanes(img, AVIF_PLANES_YUV) != AVIF_RESULT_OK) return;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            img->yuvPlanes[0][y * img->yuvRowBytes[0] + x] = 100; /* Y  */
            img->yuvPlanes[1][y * img->yuvRowBytes[1] + x] = 150; /* Cb */
            img->yuvPlanes[2][y * img->yuvRowBytes[2] + x] = 180; /* Cr */
        }
    }
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, img);
    rgb.format = AVIF_RGB_FORMAT_RGB;
    avifRGBImageAllocatePixels(&rgb);
    avifResult res = avifImageYUVToRGB(img, &rgb);
    if (res == AVIF_RESULT_OK)
        printf("CP=%u TC=%u MC=%u %s -> R=%d G=%d B=%d\n", cp, tc, mc,
               range == AVIF_RANGE_FULL ? "full" : "limited",
               rgb.pixels[0], rgb.pixels[1], rgb.pixels[2]);
    else
        printf("CP=%u TC=%u MC=%u %s -> %s\n", cp, tc, mc,
               range == AVIF_RANGE_FULL ? "full" : "limited", avifResultToString(res));
    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(img);
}
```

编译：`cc -I include probe.c -L build -lavif -o probe`，运行：`DYLD_LIBRARY_PATH=build ./probe`
（Linux 用 `-Wl,-rpath,$PWD/build` 或 `LD_LIBRARY_PATH=build`）。

用它就能复现第 4.4 节整张表。涉及文件字节的实验（2.1、2.2）另需编码 + 改字节：

1. 编码一份 4:2:0 limited、CICP 1/13/6 的最小图，用 `avifEncoderWrite()` 输出；
2. 在输出字节流里定位 4 字节标签 `colr`（紧随其后的是 4 字节 `nclx`）。以 `colr` 首个字节为 0 计：CP 在 +8/+9，TC 在 +10/+11，MC 在 +12/+13，`full_range_flag` 在 +14 的最高位。只改这些字节，OBU 里的 `mdat` 保持不动；
3. `avifDecoderReadMemory()` + `avifImageYUVToRGB()` 观察结果，与表 2.1 对照。

各条实验的预期结果：

1. 只改 `colorPrimaries` / `transferCharacteristics`，`matrixCoefficients` 固定为 6 → **RGB 输出不变**（证明 TC/CP 不参与 YUV→RGB）；
2. 只改 `matrixCoefficients` 6→1 → 181,47,142 → 191,65,144（证明 MC 决定数值）；
3. 只改 `yuvRange` limited→full → 181,47,142 → 173,55,139；
4. 只改 MC 为 12（色度派生）→ CP 从 1 改到 12/9 时输出随之变化（191/189/185），这是 CP 唯一影响矩阵的场合；
5. 编码出文件后把 `colr` 的 `nclx` 的 MC 字节改成 1（OBU 不动）→ 解码后 MC 变成 1，RGB 变化（证明 `nclx` 覆盖码流）；
6. 把 `nclx` 的 `colour_type` 改成 `free` → 解码后 CICP 回落到 OBU 的值（证明回退路径）；
7. 给同一文件加 ICC → RGB 不变（证明 ICC 不参与矩阵）。

若构建时打开 libyuv，内置浮点路径会被替换，个别码值可能相差 1（`include/avif/avif.h:945-950` 提到 libyuv "may have slightly less precision"）；结论与优先级不受影响。
