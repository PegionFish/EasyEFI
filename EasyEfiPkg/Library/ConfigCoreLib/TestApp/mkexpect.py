#!/usr/bin/env python3
"""S03 冒烟 expect 生成器：固定标记模式 + 黄金 DOM dumps 整行（regex 转义）。
用法: python3 mkexpect.py > expect-smoke.txt
DOM 行来源 tests/golden-dom.txt（宿主适配器产出；EDK2 侧 QEMU 打印同串 → T03-15 同构实证）。
"""
import re
import pathlib

here = pathlib.Path(__file__).resolve().parent
dump = (here.parent / "tests" / "golden-dom.txt").read_text().strip()

patterns = [
    r"\[S03\] ConfigSmoke start arch=(x64|aarch64)",
    r"\[S03\] GOOD items=9 errs=0 warns=0 st=0",
    r"\[S03\] GOOD PASS",
    r"\[S03\] DOM " + re.escape(dump),
    r"\[S03\] BAD items=2 errs=6 warns=3 st=0",
    r"\[S03\] BAD err idx=0 field=id code=2",
    r"\[S03\] BAD err idx=1 field=name code=3",
    r"\[S03\] BAD err idx=2 field=arch code=5",
    r"\[S03\] BAD err idx=4 field=id code=7",
    r"\[S03\] BAD err idx=5 field=path code=6",
    r"\[S03\] BAD err idx=6 field=action code=5",
    r"\[S03\] BAD warn idx=-1 field=skip_timeout code=9",
    r"\[S03\] BAD warn idx=-1 field=extra_top code=0",
    r"\[S03\] BAD warn idx=7 field=unknown_field code=0",
    r"\[S03\] BAD PASS",
    r"\[S03\] DANGER-CONFIRM PASS",
    r"\[S03\] SYNTAX PASS",
    r"\[S03\] SMOKE PASS",
]
print("\n".join(patterns))