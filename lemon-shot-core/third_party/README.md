# 第三方组件

| 目录 | 版本 | 许可 | 来源 | 用途 |
|---|---|---|---|---|
| `ncnn/` | 20261020 | BSD-3-Clause（见 LICENSE.txt） | https://github.com/Tencent/ncnn | Windows 端 OCR 推理。已删去 riscv、mips、loongarch、vulkan 层实现及工具、示例、测试 |
| `zxing-cpp/` | 3.1.1 | Apache-2.0 | https://github.com/zxing-cpp/zxing-cpp | Windows 端二维码/条码识别（只编译读取部分） |
| `libwebp/` | 1.6.0 | BSD-3-Clause（见 COPYING、PATENTS） | https://chromium.googlesource.com/webm/libwebp | 两端保存 WebP（只保留编码所需源码） |
| `ppocr/` | PP-OCRv5 mobile | Apache-2.0 | 权重来自 https://github.com/PaddlePaddle/PaddleOCR ，ncnn 格式由 https://github.com/nihui/ncnn-android-ppocrv5 转换；字典 `ppocrv5_dict.txt` 来自 PaddleOCR | Windows 端 OCR 模型，编进 exe |

更新组件时整目录替换，并按上表重新裁剪。
